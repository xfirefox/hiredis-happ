#ifdef _MSC_VER
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

#include <algorithm>
#include <cstdio>
#include <random>
#include <sstream>
#include <ctime>
#include <assert.h>
#include <detail/happ_cmd.h>

#include "detail/crc16.h"
#include "detail/happ_cluster.h"

namespace hiredis {
    namespace happ {
        namespace detail {
            static int random() {
#if defined(__cplusplus) && __cplusplus >= 201103L
                static std::mt19937 g;
                return static_cast<int>(g());
#else
                static bool inited = false;
                if (!inited) {
                    inited = true;
                    srand(time(NULL));
                }

                return rand();
#endif
            }

            static char NONE_MSG[] = "none";
        }

        cluster::cluster(): slot_flag(slot_status::INVALID) {
            conf.log_fn_debug = conf.log_fn_info = NULL;
            conf.log_buffer = NULL;
            conf.log_max_size = 0;
            conf.timer_interval_sec = HIREDIS_HAPP_TIMER_INTERVAL_SEC;
            conf.timer_interval_usec = HIREDIS_HAPP_TIMER_INTERVAL_USEC;
            conf.timer_timeout_sec = HIREDIS_HAPP_TIMER_TIMEOUT_SEC;
            conf.cmd_buffer_size = 0;

            for (int i = 0; i < HIREDIS_HAPP_SLOT_NUMBER; ++ i) {
                slots[i].index = i;
            }

            memset(&callbacks, 0, sizeof(callbacks));

            timer_actions.last_update_sec = 0;
            timer_actions.last_update_usec = 0;
        }

        cluster::~cluster() {
            reset();

            // log buffer
            if (NULL != conf.log_buffer) {
                free(conf.log_buffer);
                conf.log_buffer = NULL;
            }
        }

        int cluster::init(const std::string& ip, uint16_t port) {
            connection::set_key(conf.init_connection, ip, port);
            
            return error_code::REDIS_HAPP_OK;
        }

        int cluster::start() {
            reload_slots();
            return error_code::REDIS_HAPP_OK;
        }

        int cluster::reset() {
            std::vector<redisAsyncContext*> all_contexts;
            all_contexts.reserve(connections.size());

            // 先预存所有连接，再关闭
            {
                connection_map_t::const_iterator it_b = connections.begin();
                connection_map_t::const_iterator it_e = connections.end();
                for (; it_b != it_e; ++ it_b) {
                    if (NULL != it_b->second->get_context()) {
                        all_contexts.push_back(it_b->second->get_context());
                    }
                }
            }

            // disable slot update
            slot_flag = slot_status::UPDATING;

            // disconnect all connections. 
            // the connected/disconnected callback will be triggered if not in callback 
            for (size_t i = 0; i < all_contexts.size(); ++ i) {
                redisAsyncDisconnect(all_contexts[i]);
            }

            // 释放slot pending list
            while(!slot_pending.empty()) {
                cmd_t* cmd = slot_pending.front();
                slot_pending.pop_front();

                call_cmd(cmd, error_code::REDIS_HAPP_SLOT_UNAVAILABLE, NULL, NULL);
                destroy_cmd(cmd);
            }

            for (int i = 0; i < HIREDIS_HAPP_SLOT_NUMBER; ++i) {
                slots[i].hosts.clear();
            }


            // 释放timer pending list
            while(!timer_actions.timer_pending.empty()) {
                cmd_t* cmd = timer_actions.timer_pending.front().cmd;
                timer_actions.timer_pending.pop_front();

                call_cmd(cmd, error_code::REDIS_HAPP_TIMEOUT, NULL, NULL);
                destroy_cmd(cmd);
            }

            // connection timeout
            //while(!timer_actions.timer_conns.empty()) {
            //    timer_t::conn_timetout_t& conn_expire = timer_actions.timer_conns.front();

            //    connection_t* conn = get_connection(conn_expire.name);
            //    if (NULL != conn && conn->get_sequence() == conn_expire.sequence) {
            //        // if connection is in callback mode, the cmds in it will not finish
            //        // so the connection can not be released right now
            //        // this will be released after callback in disconnect event
            //        if (!(conn->get_context()->c.flags & REDIS_IN_CALLBACK)) {
            //            release_connection(conn->get_key(), true, error_code::REDIS_HAPP_TIMEOUT);
            //        }
            //    }

            //    timer_actions.timer_conns.pop_front();
            //}

            // all connections are marked disconnection or disconnected, so timeout timers are useless
            timer_actions.timer_conns.clear();
            timer_actions.last_update_sec = 0;
            timer_actions.last_update_usec = 0;

            // If in a callback, cmds in this connection will not finished, so it can not be freed.
            // In this case, it will call disconnect callback after callback is finished and then release the connection.
            // If not in a callback, this connection is already freed at the begining "redisAsyncDisconnect(all_contexts[i]);"  
            // connections.clear();  // can not clear connections here

            // reset slot status
            slot_flag = slot_status::INVALID;

            return 0;
        }

        cluster::cmd_t* cluster::exec(const char* key, size_t ks, cmd_t::callback_fn_t cbk, void* priv_data, int argc, const char** argv, const size_t* argvlen) {
            cmd_t* cmd = create_cmd(cbk, priv_data);
            if (NULL == cmd) {
                return NULL;
            }

            int len = cmd->vformat(argc, argv, argvlen);
            if (len <= 0) {
                log_info("format cmd with argc=%d failed", argc);
                destroy_cmd(cmd);
                return NULL;
            }

            return exec(key, ks, cmd);
        }

        cluster::cmd_t* cluster::exec(const char* key, size_t ks, cmd_t::callback_fn_t cbk, void* priv_data, const char* fmt, ...) {
            cmd_t* cmd = create_cmd(cbk, priv_data);
            if (NULL == cmd) {
                return NULL;
            }

            va_list ap;
            va_start(ap, fmt);
            int len = cmd->vformat(fmt, ap);
            va_end(ap);
            if (len <= 0) {
                log_info("format cmd with format=%s failed", fmt);
                destroy_cmd(cmd);
                return NULL;
            }

            return exec(key, ks, cmd);
        }

        cluster::cmd_t* cluster::exec(const char* key, size_t ks, cmd_t::callback_fn_t cbk, void* priv_data, const char* fmt, va_list ap) {
            cmd_t* cmd = create_cmd(cbk, priv_data);
            if (NULL == cmd) {
                return NULL;
            }

            int len = cmd->vformat(fmt, ap);
            if (len <= 0) {
                log_info("format cmd with format=%s failed", fmt);
                destroy_cmd(cmd);
                return NULL;
            }

            return exec(key, ks, cmd);
        }

        cluster::cmd_t* cluster::exec(const char* key, size_t ks, cmd_t* cmd) {
            if (NULL == cmd) {
                return NULL;
            }

            // 需要在这里转发cmd_t的所有权
            if (NULL != key && 0 != ks) {
                cmd->engine.slot = static_cast<int>(crc16(key, ks) % HIREDIS_HAPP_SLOT_NUMBER);
            }

            // ttl 预判定
            if (0 == cmd->ttl) {
                log_debug("cmd %p at slot %d ttl expired", cmd, cmd->engine.slot);
                call_cmd(cmd, error_code::REDIS_HAPP_TTL, NULL, NULL);
                destroy_cmd(cmd);
                return NULL;
            }

            // update slot
            if (slot_status::INVALID == slot_flag || slot_status::UPDATING == slot_flag) {
                log_debug("transfer cmd at slot %d to slot update pending list", cmd->engine.slot);
                slot_pending.push_back(cmd);

                reload_slots();
                return cmd;
            }

            // 指定或随机获取服务器地址
            const connection::key_t* conn_key = get_slot_master(cmd->engine.slot);

            if (NULL == conn_key) {
                log_info("get connect of slot %d failed", cmd->engine.slot);
                call_cmd(cmd, error_code::REDIS_HAPP_CONNECTION, NULL, NULL);
                destroy_cmd(cmd);
                return NULL;
            }

            // 转发到建立连接
            connection_t* conn_inst = get_connection(conn_key->name);
            if (NULL == conn_inst) {
                conn_inst = make_connection(*conn_key);
            }

            if (NULL == conn_inst) {
                log_info("connect to %s failed", conn_key->name.c_str());

                call_cmd(cmd, error_code::REDIS_HAPP_CONNECTION, NULL, NULL);
                destroy_cmd(cmd);

                return NULL;
            }

            return exec(conn_inst, cmd);
        }

        cluster::cmd_t* cluster::exec(connection_t* conn, cmd_t* cmd) {
            if (NULL == cmd) {
                return NULL;
            }

            // ttl 正式判定
            if (0 == cmd->ttl) {
                log_debug("cmd %p at slot %d ttl expired", cmd, cmd->engine.slot);
                call_cmd(cmd, error_code::REDIS_HAPP_TTL, NULL, NULL);
                destroy_cmd(cmd);
                return NULL;
            }

            // ttl
            --cmd->ttl;

            if (NULL == conn) {
                call_cmd(cmd, error_code::REDIS_HAPP_CONNECTION, NULL, NULL);
                destroy_cmd(cmd);
                return NULL;
            }

            // 主循环逻辑回包处理
            int res = conn->redis_cmd(cmd, on_reply_wrapper);

            if (REDIS_OK != res) {
                // hiredis的代码，仅在网络关闭和命令错误会返回出错
                // 其他情况都应该直接出错回调
                if (conn->get_context()->c.flags & (REDIS_DISCONNECTING | REDIS_FREEING)) {
                    // 尝试释放连接信息,避免下一次使用无效连接
                    remove_connection_key(conn->get_key().name);

                    // Fix hiredis 某个版本 的BUG，可能会漏调用onDisconnect
                    // 只要不在hiredis的回调函数内，一旦标记了REDIS_DISCONNECTING或REDIS_FREEING则是已经释放完毕了
                    // 如果是回调函数，则出回调以后会调用disconnect，从而触发disconnect回调，这里就不需要释放了
                    if (!(conn->get_context()->c.flags & REDIS_IN_CALLBACK)) {
                        release_connection(conn->get_key(), false, error_code::REDIS_HAPP_CONNECTION);
                    }

                    // conn = NULL;
                    // 连接丢失需要重连，先随机重新找可用连接
                    cmd->engine.slot = -1;
                    return retry(cmd, NULL);
                } else {
                    call_cmd(cmd, error_code::REDIS_HAPP_HIREDIS, conn->get_context(), NULL);
                    destroy_cmd(cmd);
                }
                return NULL;
            }

            log_debug("exec cmd %p at slot %d, connection %s", cmd, cmd->engine.slot, conn->get_key().name.c_str());
            return cmd;
        }

        cluster::cmd_t* cluster::retry(cmd_t* cmd, connection_t* conn) {
            // 重试次数较少则直接重试
            if(NULL == cmd) {
                return NULL;
            }

            if (false == is_timer_active() || cmd->ttl > HIREDIS_HAPP_TTL / 2) {
                if (NULL == conn) {
                    return exec(NULL, 0, cmd);
                } else {
                    return exec(conn, cmd);
                }
            }

            // 重试次数较多则等一会重试
            // 延迟重试的命令不记录连接信息，因为可能到时候连接已经丢失
            add_timer_cmd(cmd);
            return cmd;
        }

        bool cluster::reload_slots() {
            if (slot_status::UPDATING == slot_flag) {
                return false;
            }

            const connection::key_t* conn_key = get_slot_master(-1);
            if (NULL == conn_key) {
                return false;
            }

            connection_t* conn = get_connection(conn_key->name);
            if (NULL == conn) {
                conn = make_connection(*conn_key);
            }

            if (NULL == conn) {
                return false;
            }

            // CLUSTER SLOTS cmd
            cmd_t* cmd = create_cmd(on_reply_update_slot, NULL);
            if (NULL == cmd) {
                return error_code::REDIS_HAPP_CREATE;
            }

            int len = cmd->format("CLUSTER SLOTS");
            if (len <= 0) {
                log_info("format cmd CLUSTER SLOTS failed");
                destroy_cmd(cmd);
                return false;
            }

            if(NULL != exec(conn, cmd)) {
                slot_flag = slot_status::UPDATING;
            }

            return true;
        }

        const connection::key_t* cluster::get_slot_master(int index) {
            if (index >= 0 && index < HIREDIS_HAPP_SLOT_NUMBER && !slots[index].hosts.empty()) {
                return &slots[index].hosts.front();
            }

            // 随机获取地址
            index = (detail::random() & 0xFFFF) % HIREDIS_HAPP_SLOT_NUMBER;
            if (slots[index].hosts.empty()) {
                return &conf.init_connection;
            }

            return &slots[index].hosts.front();
        }
        
        const cluster::slot_t* cluster::get_slot_by_key(const char* key, size_t ks) const {
            int index = static_cast<int>(crc16(key, ks) % HIREDIS_HAPP_SLOT_NUMBER);
            return &slots[index];
        }

        const cluster::connection_t* cluster::get_connection(const std::string& key) const {
            connection_map_t::const_iterator it = connections.find(key);
            if (it == connections.end()) {
                return NULL;
            }

            return it->second.get();
        }

        cluster::connection_t* cluster::get_connection(const std::string& key) {
            connection_map_t::iterator it = connections.find(key);
            if (it == connections.end()) {
                return NULL;
            }

            return it->second.get();
        }

        const cluster::connection_t* cluster::get_connection(const std::string& ip, uint16_t port) const {
            return get_connection(connection::make_name(ip, port));
        }

        cluster::connection_t* cluster::get_connection(const std::string& ip, uint16_t port) {
            return get_connection(connection::make_name(ip, port));
        }

        cluster::connection_t* cluster::make_connection(const connection::key_t& key) {
            holder_t h;
            connection_map_t::iterator check_it = connections.find(key.name);
            if ( check_it != connections.end()) {
                log_debug("connection %s already exists", key.name.c_str());
                return NULL;
            }

            redisAsyncContext* c = redisAsyncConnect(key.ip.c_str(), static_cast<int>(key.port));
            if (NULL == c || c->err) {
                log_info("redis connect to %s failed, msg: %s", key.name.c_str(), NULL == c? detail::NONE_MSG: c->errstr);
                return NULL;
            }

            h.clu = this;
            redisAsyncSetConnectCallback(c, on_connected_wrapper);
            redisAsyncSetDisconnectCallback(c, on_disconnected_wrapper);
            redisEnableKeepAlive(&c->c);
            if (conf.timer_timeout_sec > 0) {
                struct timeval tv;
                tv.tv_sec =conf.timer_timeout_sec;
                tv.tv_usec = 0;
                redisSetTimeout(&c->c, tv);
            }

            ::hiredis::happ::unique_ptr<connection_t>::type ret_ptr(new connection_t());
            connection_t& ret = *ret_ptr;
            ::hiredis::happ::unique_ptr<connection_t>::swap(connections[key.name], ret_ptr);
            ret.init(h, key);
            ret.set_connecting(c);

            c->data = &ret;

            // timeout timer
            if(conf.timer_timeout_sec > 0 && is_timer_active()) {
                timer_actions.timer_conns.push_back(timer_t::conn_timetout_t());
                timer_t::conn_timetout_t& conn_expire = timer_actions.timer_conns.back();
                conn_expire.name = key.name;
                conn_expire.sequence = ret.get_sequence();
                conn_expire.timeout = timer_actions.last_update_sec + conf.timer_timeout_sec;
            }

            // event callback
            if (callbacks.on_connect) {
                callbacks.on_connect(this, &ret);
            }

            log_debug("redis make connection to %s ", key.name.c_str());
            return &ret;
        }

        bool cluster::release_connection(const connection::key_t& key, bool close_fd, int status) {
            connection_map_t::iterator it = connections.find(key.name);
            if (connections.end() == it) {
                log_debug("connection %s not found", key.name.c_str());
                return false;
            }

            connection_t::status::type from_status = it->second->set_disconnected(close_fd);
            switch(from_status) {
                // 递归调用，直接退出
                case connection_t::status::DISCONNECTED:
                    return true;

                // 正在连接，响应connected事件
                case connection_t::status::CONNECTING:
                    if(callbacks.on_connected) {
                        callbacks.on_connected(this, it->second.get(), it->second->get_context(),
                            error_code::REDIS_HAPP_OK == status? error_code::REDIS_HAPP_CONNECTION: status
                        );
                    }
                    break;

                // 已连接，响应disconnected事件
                case connection_t::status::CONNECTED:
                    if(callbacks.on_disconnected) {
                        callbacks.on_disconnected(this, it->second.get(), it->second->get_context(), status);
                    }
                    break;

                default:
                    log_info("unknown connection status %d", static_cast<int>(from_status));
                    break;
            }

            log_debug("release connection %s", key.name.c_str());

            // can not use key any more
            connections.erase(it);
            
            return true;
        }

        cluster::onconnect_fn_t cluster::set_on_connect(onconnect_fn_t cbk) {
            using std::swap;
            swap(cbk, callbacks.on_connect);
            return cbk;
        }

        cluster::onconnected_fn_t cluster::set_on_connected(onconnected_fn_t cbk) {
            using std::swap;
            swap(cbk, callbacks.on_connected);
            return cbk;
        }

        cluster::ondisconnected_fn_t cluster::set_on_disconnected(ondisconnected_fn_t cbk) {
            using std::swap;
            swap(cbk, callbacks.on_disconnected);
            return cbk;
        }

        void cluster::set_cmd_buffer_size(size_t s) {
            conf.cmd_buffer_size = s;
        }

        size_t cluster::get_cmd_buffer_size() const {
            return conf.cmd_buffer_size;
        }

        bool cluster::is_timer_active() const {
            return (timer_actions.last_update_sec != 0 || timer_actions.last_update_usec != 0) &&
                (conf.timer_interval_sec > 0 || conf.timer_interval_usec > 0);
        }

        void cluster::set_timer_interval(time_t sec, time_t usec) {
            conf.timer_interval_sec = sec;
            conf.timer_interval_usec = usec;
        }

        void cluster::set_timeout(time_t sec) {
            conf.timer_timeout_sec = sec;
        }

        void cluster::add_timer_cmd(cmd_t* cmd) {
            if (NULL == cmd) {
                return;
            }

            if (is_timer_active()) {
                timer_actions.timer_pending.push_back(timer_t::delay_t());
                timer_t::delay_t& d = timer_actions.timer_pending.back();
                d.sec = timer_actions.last_update_sec + conf.timer_interval_sec;
                d.usec = timer_actions.last_update_usec + conf.timer_interval_usec;
                d.cmd = cmd;
            } else {
                exec(NULL, 0, cmd);
            }
        }

        int cluster::proc(time_t sec, time_t usec) {
            int ret = 0;

            timer_actions.last_update_sec = sec;
            timer_actions.last_update_usec = usec;

            while (!timer_actions.timer_pending.empty()) {
                timer_t::delay_t& rd = timer_actions.timer_pending.front();
                if (rd.sec > sec || (rd.sec == sec && rd.usec > usec)) {
                    break;
                }


                timer_t::delay_t d = rd;
                timer_actions.timer_pending.pop_front();

                exec(NULL, 0, d.cmd);

                ++ret;
            }

            // connection timeout
            // this can not be call in callback
            while(!timer_actions.timer_conns.empty() && sec >= timer_actions.timer_conns.front().timeout) {
                timer_t::conn_timetout_t& conn_expire = timer_actions.timer_conns.front();

                connection_t* conn = get_connection(conn_expire.name);
                if (NULL != conn && conn->get_sequence() == conn_expire.sequence) {
                    assert(!(conn->get_context()->c.flags & REDIS_IN_CALLBACK));
                    release_connection(conn->get_key(), true, error_code::REDIS_HAPP_TIMEOUT);
                }

                timer_actions.timer_conns.pop_front();
            }

            return ret;
        }

        cluster::cmd_t* cluster::create_cmd(cmd_t::callback_fn_t cbk, void* pridata) {
            holder_t h;
            h.clu = this;
            cmd_t* ret = cmd_t::create(h, cbk, pridata, conf.cmd_buffer_size);
            return ret;
        }

        void cluster::destroy_cmd(cmd_t* c) {
            if (NULL == c) {
                log_debug("can not destroy null cmd");
                return;
            }

            // 丢失连接
            if (NULL != c->callback) {
                call_cmd(c, error_code::REDIS_HAPP_UNKNOWD, NULL, NULL);
            }

            cmd_t::destroy(c);
        }

        int cluster::call_cmd(cmd_t* c, int err, redisAsyncContext* context, void* reply) {
            if (NULL == c) {
                log_debug("can not call cmd without cmd object");
                return error_code::REDIS_HAPP_UNKNOWD;
            }

            return c->call_reply(err, context, reply);
        }

        void cluster::set_log_writer(log_fn_t info_fn, log_fn_t debug_fn, size_t max_size) {
            using std::swap;
            conf.log_fn_info = info_fn;
            conf.log_fn_debug = debug_fn;
            conf.log_max_size = max_size;

            if (NULL != conf.log_buffer) {
                free(conf.log_buffer);
                conf.log_buffer = NULL;
            }
        }

        void cluster::on_reply_wrapper(redisAsyncContext* c, void* r, void* privdata) {
            connection_t* conn = reinterpret_cast<connection_t*>(c->data);
            cmd_t* cmd = reinterpret_cast<cmd_t*>(privdata);
            cluster* self = cmd->holder.clu;

            // 正在释放的连接重试也只会死循环，所以直接失败退出
            if (c->c.flags & REDIS_DISCONNECTING) {
                self->log_debug("redis cmd %p reply when disconnecting context err %d,msg %s", cmd, c->err, NULL == c->errstr? detail::NONE_MSG: c->errstr);
                cmd->err = error_code::REDIS_HAPP_CONNECTION;
                conn->call_reply(cmd, r);
                return;
            }

            if (REDIS_ERR_IO == c->err && REDIS_ERR_EOF == c->err) {
                self->log_debug("redis cmd %p reply context err %d and will retry, %s", cmd, c->err, c->errstr);
                // 网络错误则重试
                conn->pop_reply(cmd);
                self->retry(cmd);
                return;
            }

            if (REDIS_OK != c->err || NULL == r) {
                self->log_debug("redis cmd %p reply context err %d and abort, %s", cmd, c->err, NULL == c->errstr? detail::NONE_MSG: c->errstr);
                // 其他错误则向上传递
                conn->call_reply(cmd, r);
                return;
            }

            redisReply* reply = reinterpret_cast<redisReply*>(r);

            // 错误处理
            if (REDIS_REPLY_ERROR == reply->type) {
                int slot_index = 0;
                char addr[260] = { 0 };

                // 检测 MOVED，ASK和CLUSTERDOWN指令
                if(0 == HIREDIS_HAPP_STRNCASE_CMP("ASK", reply->str, 3)) {
                    self->log_debug("redis cmd %p %s", cmd, reply->str);
                    // 发送ASK到目标connection
                    HIREDIS_HAPP_SSCANF(reply->str + 4, " %d %s", &slot_index, addr);
                    std::string ip;
                    uint16_t port;
                    if (connection::pick_name(addr, ip, port)) {
                        connection::key_t conn_key;
                        connection::set_key(conn_key, ip, port);

                        // ASKING 请求
                        connection_t* ask_conn = self->get_connection(conn_key.name);
                        if (NULL == ask_conn) {
                            ask_conn = self->make_connection(conn_key);
                        }

                        // cmd转移到新的connection，并在完成后执行
                        conn->pop_reply(cmd);

                        if (NULL != ask_conn) {
                            // 先从原始conn队列弹出
                            if(REDIS_OK == redisAsyncCommand(ask_conn->get_context(), on_reply_asking, cmd, "ASKING")) {
                                return;
                            }
                        }

                        // ASK 不成功则重试
                        self->retry(cmd);
                        return;
                    }
                } else if (0 == HIREDIS_HAPP_STRNCASE_CMP("MOVED", reply->str, 5)) {
                    self->log_debug("redis cmd %p %s", cmd, reply->str);

                    HIREDIS_HAPP_SSCANF(reply->str + 6, " %d %s", &slot_index, addr);

                    if (cmd->engine.slot >= 0 && cmd->engine.slot != slot_index) {
                        self->log_info("cluster cmd key error, expect slot: %d, real slot: %d", cmd->engine.slot, slot_index);
                        cmd->engine.slot = slot_index;
                    }

                    std::string ip;
                    uint16_t port;
                    if (connection::pick_name(addr, ip, port)) {
                        // 更新一条slot
                        self->slots[slot_index].hosts.clear();
                        self->slots[slot_index].hosts.push_back(connection::key_t());
                        connection::set_key(self->slots[slot_index].hosts.back(), ip, port);

                        // retry
                        conn->pop_reply(cmd);
                        self->retry(cmd);

                        // 重新拉取slot列表
                        // TODO 这里是否要强制拉取slots列表？
                        // 如果不拉取可能丢失从节点信息，但是拉取的话迁移过程中可能会导致更新多次？
                        // 并且更新slots也是一个比较耗费CPU的操作（16384次list的清空和复制）
                        self->reload_slots();
                        return;
                    } else {
                        self->slot_flag = slot_status::INVALID;
                    }
                } else if (0 == HIREDIS_HAPP_STRNCASE_CMP("CLUSTERDOWN", reply->str, 11)) {
                    self->log_info("cluster down reset all connection, cmd and replys");
                    conn->call_reply(cmd, r);
                    self->reset();
                    return;
                }

                self->log_debug("redis cmd %p reply error and abort, msg: %s", cmd, NULL == reply->str? detail::NONE_MSG: reply->str);
                // 其他错误则向上传递
                conn->call_reply(cmd, r);
                return;
            }

            // 正常回调
            self->log_debug("redis cmd %p got reply success.(ttl=%3d)", cmd, NULL == cmd ? -1: static_cast<int>(cmd->ttl));
            conn->call_reply(cmd, r);
        }

        void cluster::on_reply_update_slot(cmd_exec* cmd, redisAsyncContext*, void* r, void* privdata) {
            redisReply* reply = reinterpret_cast<redisReply*>(r);
            cluster* self = cmd->holder.clu;

            // 出错，重新拉取
            if (NULL == reply || reply->elements <= 0 || REDIS_REPLY_ARRAY != reply->element[0]->type) {
                self->slot_flag = slot_status::INVALID;

                if (!self->slot_pending.empty()) {
                    self->log_info("update slots failed and try to retry again.");

                    // 断线则稍后重试
                    // 如果没有开启定时器且只有一条连接且该链接处于关闭状态
                    // 这条消息将会重试TTL次后失败
                    // 更新slots的命令永远随机slot，不缓存连接信息
                    cmd->engine.slot = -1;
                    self->add_timer_cmd(cmd);
                } else {
                    self->log_info("update slots failed and will retry later.");
                }
                
                return;
            }

            // clear and reset slots ... 
            for (size_t i = 0; i < HIREDIS_HAPP_SLOT_NUMBER; ++ i) {
                self->slots[i].hosts.clear();
            }

            for (size_t i = 0; i < reply->elements; ++ i) {
                redisReply* slot_node = reply->element[i];
                if (slot_node->elements >= 3) {
                    long long si = slot_node->element[0]->integer;
                    long long ei = slot_node->element[1]->integer;

                    std::vector<connection::key_t> hosts;
                    for (size_t j = 2; j < slot_node->elements; ++ j) {
                        redisReply* addr = slot_node->element[j];
                        // cluster 数据不正常时，redis可能返回空地址。这时候要忽略
                        if (addr->elements >= 2 &&
                            REDIS_REPLY_STRING == addr->element[0]->type && addr->element[0]->str[0] &&
                            REDIS_REPLY_INTEGER == addr->element[1]->type) {
                            hosts.push_back(connection::key_t());
                            connection::set_key(
                                hosts.back(),
                                addr->element[0]->str,
                                static_cast<uint16_t>(addr->element[1]->integer)
                            );
                        }
                        
                    }

                    // log
                    if(NULL != self->conf.log_fn_debug && self->conf.log_max_size > 0 ) {
                        self->log_debug("slot update: [%lld-%lld]", si, ei);
                        for (size_t j = 0; j < hosts.size(); ++ j) {
                            self->log_debug(" -- %s", hosts[j].name.c_str());
                        }
                    }
                    // 16384次复制
                    for (; si <= ei; ++ si) {
                        self->slots[si].hosts = hosts;
                    }
                }
            }

            // 先设置状态，然后重试命令。否则会陷入死循环
            self->slot_flag = slot_status::OK;
            
            self->log_info("update %d slots done",static_cast<int>(reply->elements));

            // 执行待执行队列
            while(!self->slot_pending.empty()) {
                cmd_t* cmd = self->slot_pending.front();
                self->slot_pending.pop_front();
                self->retry(cmd);
            }
        }

        void cluster::on_reply_asking(redisAsyncContext* c, void* r, void* privdata) {
            cmd_t* cmd = reinterpret_cast<cmd_t*>(privdata);
            redisReply* reply = reinterpret_cast<redisReply*>(r);
            connection_t* conn = reinterpret_cast<connection_t*>(c->data);
            cluster* self = conn->get_holder().clu;

            // ask命令带回的cmd不在任何conn队列中
            // 所以不需要弹出cmd，直接retry或者调用回调即可

            if (REDIS_ERR_IO == c->err && REDIS_ERR_EOF == c->err) {
                self->log_debug("redis asking err %d and will retry, %s", c->err, c->errstr);
                // 网络错误则重试
                self->retry(cmd);
                return;
            }

            if (REDIS_OK != c->err || NULL == r) {
                self->log_debug("redis asking err %d and abort, %s", c->err, NULL == c->errstr ? detail::NONE_MSG : c->errstr);

                if(c->c.flags & REDIS_DISCONNECTING) {
                    cmd->err = error_code::REDIS_HAPP_CONNECTION;
                }

                // 其他错误则向上传递
                self->call_cmd(cmd, error_code::REDIS_HAPP_CONNECTION, conn->get_context(), r);
                self->destroy_cmd(cmd);
                return;
            }

            if (NULL != reply->str && 0 == HIREDIS_HAPP_STRNCASE_CMP("OK", reply->str, 2)) {
                self->retry(cmd, conn);
                return;
            }

            self->log_debug("redis reply asking err %d and abort, %s", reply->type, NULL == reply->str ? detail::NONE_MSG : reply->str);
            // 其他错误则向上传递
            self->call_cmd(cmd, error_code::REDIS_HAPP_CONNECTION, conn->get_context(), r);
            self->destroy_cmd(cmd);
        }

        void cluster::on_connected_wrapper(const struct redisAsyncContext* c, int status) {
            connection_t* conn = reinterpret_cast<connection_t*>(c->data);
            cluster* self = conn->get_holder().clu;
            
            // event callback
            if (self->callbacks.on_connected) {
                self->callbacks.on_connected(self, conn, c, status);
            }

            // 失败则释放资源
            if (REDIS_OK != status) {
                self->log_debug("connect to %s failed, status: %d, msg: %s", conn->get_key().name.c_str(), status, c->errstr);
                self->release_connection(conn->get_key(), false, status);

                // 连接失败则更新slot列表
                self->reload_slots();
            } else {
                conn->set_connected();

                self->log_debug("connect to %s success", conn->get_key().name.c_str());

                // 更新slot
                if (slot_status::INVALID == self->slot_flag) {
                    self->reload_slots();
                }
            }

        }

        void cluster::on_disconnected_wrapper(const struct redisAsyncContext* c, int status) {
            connection_t* conn = reinterpret_cast<connection_t*>(c->data);
            cluster* self = conn->get_holder().clu;

            // 如果网络错误断开，则下一次命令需要更新slots
            if(REDIS_OK != status) {
                self->remove_connection_key(conn->get_key().name);
            }

            // 释放资源
            self->release_connection(conn->get_key(), false, status);
        }

        void cluster::remove_connection_key(const std::string& name) {
            slot_flag = slot_status::INVALID;

            for (int i = 0; i < HIREDIS_HAPP_SLOT_NUMBER; ++ i) {
                std::vector<connection::key_t>& hosts = slots[i].hosts;
                if (!hosts.empty() && hosts[0].name == name) {
                    if (hosts.size() > 1) {
                        using std::swap;
                        swap(hosts[0], hosts[hosts.size() - 1]);
                    }

                    hosts.pop_back();
                }
            }
        }

        void cluster::log_debug(const char* fmt, ...) {
            if (NULL == conf.log_fn_debug || 0 == conf.log_max_size ) {
                return;
            }

            if (NULL == conf.log_buffer) {
                conf.log_buffer = reinterpret_cast<char*>(malloc(conf.log_max_size));
            }

            va_list ap;
            va_start(ap, fmt);
            int len = vsnprintf(conf.log_buffer, conf.log_max_size, fmt, ap);
            va_end(ap);

            conf.log_buffer[conf.log_max_size - 1] = 0;
            if (len > 0) {
                conf.log_buffer[len] = 0;
            }

            conf.log_fn_debug(conf.log_buffer);
        }

        void cluster::log_info(const char* fmt, ...) {
            if (NULL == conf.log_fn_info || 0 == conf.log_max_size) {
                return;
            }

            if (NULL == conf.log_buffer) {
                conf.log_buffer = reinterpret_cast<char*>(malloc(conf.log_max_size));
            }

            va_list ap;
            va_start(ap, fmt);
            int len = vsnprintf(conf.log_buffer, conf.log_max_size, fmt, ap);
            va_end(ap);

            conf.log_buffer[conf.log_max_size - 1] = 0;
            if (len > 0) {
                conf.log_buffer[len] = 0;
            }

            conf.log_fn_info(conf.log_buffer);
        }
    }
}
