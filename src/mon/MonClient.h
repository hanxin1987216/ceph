// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_MONCLIENT_H
#define CEPH_MONCLIENT_H

#include "msg/Messenger.h"

#include "MonMap.h"

#include "common/Timer.h"
#include "common/Finisher.h"
#include "common/config.h"
#include "auth/AuthClientHandler.h"
#include "auth/RotatingKeyRing.h"
#include "common/SimpleRNG.h"


class MMonMap;
class MMonGetVersion;
class MMonGetVersionReply;
struct MMonSubscribeAck;
class MMonCommandAck;
class MCommandReply;
struct MAuthReply;
class MAuthRotating;
class MPing;
class LogClient;
class AuthSupported;
class AuthMethodList;
class Messenger;
// class RotatingKeyRing;
class KeyRing;
enum MonClientState {
  MC_STATE_NONE,
  MC_STATE_NEGOTIATING,
  MC_STATE_AUTHENTICATING,
  MC_STATE_HAVE_SESSION,
};

struct MonClientPinger : public Dispatcher {

  Mutex lock;
  Cond ping_recvd_cond;
  string *result;
  bool done;

  MonClientPinger(CephContext *cct_, string *res_) :
    Dispatcher(cct_),
    lock("MonClientPinger::lock"),
    result(res_),
    done(false)
  { }

  int wait_for_reply(double timeout = 0.0) {
    utime_t until = ceph_clock_now(cct);
    until += (timeout > 0 ? timeout : cct->_conf->client_mount_timeout);
    done = false;

    int ret = 0;
    while (!done) {
      ret = ping_recvd_cond.WaitUntil(lock, until);
      if (ret == ETIMEDOUT)
        break;
    }
    return ret;
  }

  bool ms_dispatch(Message *m) {
    Mutex::Locker l(lock);
    if (m->get_type() != CEPH_MSG_PING)
      return false;

    bufferlist &payload = m->get_payload();
    if (result && payload.length() > 0) {
      bufferlist::iterator p = payload.begin();
      ::decode(*result, p);
    }
    done = true;
    ping_recvd_cond.SignalAll();
    m->put();
    return true;
  }
  bool ms_handle_reset(Connection *con) {
    Mutex::Locker l(lock);
    done = true;
    ping_recvd_cond.SignalAll();
    return true;
  }
  void ms_handle_remote_reset(Connection *con) {}
  bool ms_handle_refused(Connection *con) {
    return false;
  }
};

class MonClient : public Dispatcher {
public:
  MonMap monmap;
private:
  MonClientState state;

  Messenger *messenger;

  string cur_mon;
  ConnectionRef cur_con;

  SimpleRNG rng;

  EntityName entity_name;

  entity_addr_t my_addr;

  Mutex monc_lock;
  SafeTimer timer;
  Finisher finisher;

  bool initialized;
  bool no_keyring_disabled_cephx;

  LogClient *log_client;
  bool more_log_pending;

  void send_log();

  AuthMethodList *auth_supported;

  bool ms_dispatch(Message *m);
  bool ms_handle_reset(Connection *con);
  void ms_handle_remote_reset(Connection *con) {}
  bool ms_handle_refused(Connection *con) { return false; }

  void handle_monmap(MMonMap *m);

  void handle_auth(MAuthReply *m);

  // monitor session
  bool hunting;

  void tick();
  void schedule_tick();

  Cond auth_cond;

  void handle_auth_rotating_response(MAuthRotating *m);
  // monclient
  bool want_monmap;

  uint32_t want_keys;

  uint64_t global_id;

  // authenticate
private:
  Cond map_cond;
  int authenticate_err;

  list<Message*> waiting_for_session;
  utime_t last_rotating_renew_sent;
  Context *session_established_context;
  bool had_a_connection;
  double reopen_interval_multiplier;

  string _pick_random_mon();
  void _finish_hunting();
  void _reopen_session(int rank, string name);
  void _reopen_session() {
    _reopen_session(-1, string());
  }
  void _send_mon_message(Message *m, bool force=false);

public:
  void set_entity_name(EntityName name) { entity_name = name; }

  int _check_auth_tickets();
  int _check_auth_rotating();
  int wait_auth_rotating(double timeout);

  int authenticate(double timeout=0.0);

  /**
   * Try to flush as many log messages as we can in a single
   * message.  Use this before shutting down to transmit your
   * last message.
   */
  void flush_log();

  // mon subscriptions
private:
  map<string,ceph_mon_subscribe_item> sub_sent; // my subs, and current versions
  map<string,ceph_mon_subscribe_item> sub_new;  // unsent new subs
  utime_t sub_renew_sent, sub_renew_after;

  void _renew_subs();
  void handle_subscribe_ack(MMonSubscribeAck* m);

  bool _sub_want(const string &what, version_t start, unsigned flags) {
    auto sub = sub_new.find(what);
    if (sub != sub_new.end() &&
        sub->second.start == start &&
        sub->second.flags == flags) {
      return false;
    } else {
      sub = sub_sent.find(what);
      if (sub != sub_sent.end() &&
	  sub->second.start == start &&
	  sub->second.flags == flags)
	return false;
    }

    sub_new[what].start = start;
    sub_new[what].flags = flags;
    return true;
  }
  void _sub_got(const string &what, version_t got) {
    if (sub_new.count(what)) {
      if (sub_new[what].start <= got) {
	if (sub_new[what].flags & CEPH_SUBSCRIBE_ONETIME)
	  sub_new.erase(what);
	else
	  sub_new[what].start = got + 1;
      }
    } else if (sub_sent.count(what)) {
      if (sub_sent[what].start <= got) {
	if (sub_sent[what].flags & CEPH_SUBSCRIBE_ONETIME)
	  sub_sent.erase(what);
	else
	  sub_sent[what].start = got + 1;
      }
    }
  }
  void _sub_unwant(const string &what) {
    sub_sent.erase(what);
    sub_new.erase(what);
  }

  // auth tickets
public:
  AuthClientHandler *auth;
public:
  void renew_subs() {
    Mutex::Locker l(monc_lock);
    _renew_subs();
  }
  bool sub_want(string what, version_t start, unsigned flags) {
    Mutex::Locker l(monc_lock);
    return _sub_want(what, start, flags);
  }
  void sub_got(string what, version_t have) {
    Mutex::Locker l(monc_lock);
    _sub_got(what, have);
  }
  void sub_unwant(string what) {
    Mutex::Locker l(monc_lock);
    _sub_unwant(what);
  }
  /**
   * Increase the requested subscription start point. If you do increase
   * the value, apply the passed-in flags as well; otherwise do nothing.
   */
  bool sub_want_increment(string what, version_t start, unsigned flags) {
    Mutex::Locker l(monc_lock);
    map<string,ceph_mon_subscribe_item>::iterator i = sub_new.find(what);
    if (i != sub_new.end()) {
      if (i->second.start >= start)
	return false;
      i->second.start = start;
      i->second.flags = flags;
      return true;
    }

    i = sub_sent.find(what);
    if (i == sub_sent.end() || i->second.start < start) {
      ceph_mon_subscribe_item& item = sub_new[what];
      item.start = start;
      item.flags = flags;
      return true;
    }
    return false;
  }
  
  KeyRing *keyring;
  RotatingKeyRing *rotating_secrets;

 public:
  explicit MonClient(CephContext *cct_);
  MonClient(const MonClient &) = delete;
  MonClient& operator=(const MonClient &) = delete;
  ~MonClient();

  int init();
  void shutdown();

  void set_log_client(LogClient *clog) {
    log_client = clog;
  }

  int build_initial_monmap();
  int get_monmap();
  int get_monmap_privately();
  /**
   * Ping monitor with ID @p mon_id and record the resulting
   * reply in @p result_reply.
   *
   * @param[in]  mon_id Target monitor's ID
   * @param[out] result_reply reply from mon.ID, if param != NULL
   * @returns    0 in case of success; < 0 in case of error,
   *             -ETIMEDOUT if monitor didn't reply before timeout
   *             expired (default: conf->client_mount_timeout).
   */
  int ping_monitor(const string &mon_id, string *result_reply);

  void send_mon_message(Message *m) {
    Mutex::Locker l(monc_lock);
    _send_mon_message(m);
  }
  /**
   * If you specify a callback, you should not call
   * reopen_session() again until it has been triggered. The MonClient
   * will behave, but the first callback could be triggered after
   * the session has been killed and the MonClient has started trying
   * to reconnect to another monitor.
   */
  void reopen_session(Context *cb=NULL) {
    Mutex::Locker l(monc_lock);
    if (cb) {
      delete session_established_context;
      session_established_context = cb;
    }
    _reopen_session();
  }

  entity_addr_t get_my_addr() const {
    return my_addr;
  }

  const uuid_d& get_fsid() {
    return monmap.fsid;
  }

  entity_addr_t get_mon_addr(unsigned i) {
    Mutex::Locker l(monc_lock);
    if (i < monmap.size())
      return monmap.get_addr(i);
    return entity_addr_t();
  }
  entity_inst_t get_mon_inst(unsigned i) {
    Mutex::Locker l(monc_lock);
    if (i < monmap.size())
      return monmap.get_inst(i);
    return entity_inst_t();
  }
  int get_num_mon() {
    Mutex::Locker l(monc_lock);
    return monmap.size();
  }

  uint64_t get_global_id() const {
    return global_id;
  }

  void set_messenger(Messenger *m) { messenger = m; }
  entity_addr_t get_myaddr() const { return messenger->get_myaddr(); }

  void send_auth_message(Message *m) {
    _send_mon_message(m, true);
  }

  void set_want_keys(uint32_t want) {
    want_keys = want;
    if (auth)
      auth->set_want_keys(want | CEPH_ENTITY_TYPE_MON);
  }

  // admin commands
private:
  uint64_t last_mon_command_tid;
  struct MonCommand {
    string target_name;
    int target_rank;
    uint64_t tid;
    vector<string> cmd;
    bufferlist inbl;
    bufferlist *poutbl;
    string *prs;
    int *prval;
    Context *onfinish, *ontimeout;

    explicit MonCommand(uint64_t t)
      : target_rank(-1),
	tid(t),
	poutbl(NULL), prs(NULL), prval(NULL), onfinish(NULL), ontimeout(NULL)
    {}
  };
  map<uint64_t,MonCommand*> mon_commands;

  void _send_command(MonCommand *r);
  void _resend_mon_commands();
  int _cancel_mon_command(uint64_t tid, int r);
  void _finish_command(MonCommand *r, int ret, string rs);
  void handle_mon_command_ack(MMonCommandAck *ack);

public:
  int start_mon_command(const vector<string>& cmd, const bufferlist& inbl,
			bufferlist *outbl, string *outs,
			Context *onfinish);
  int start_mon_command(int mon_rank,
			const vector<string>& cmd, const bufferlist& inbl,
			bufferlist *outbl, string *outs,
			Context *onfinish);
  int start_mon_command(const string &mon_name,  ///< mon name, with mon. prefix
			const vector<string>& cmd, const bufferlist& inbl,
			bufferlist *outbl, string *outs,
			Context *onfinish);

  // version requests
public:
  /**
   * get latest known version(s) of cluster map
   *
   * @param map string name of map (e.g., 'osdmap')
   * @param newest pointer where newest map version will be stored
   * @param oldest pointer where oldest map version will be stored
   * @param onfinish context that will be triggered on completion
   * @return (via context) 0 on success, -EAGAIN if we need to resubmit our request
   */
  void get_version(string map, version_t *newest, version_t *oldest, Context *onfinish);

  /**
   * Run a callback within our lock, with a reference
   * to the MonMap
   */
  template<typename Callback, typename...Args>
  auto with_monmap(Callback&& cb, Args&&...args) ->
    typename std::enable_if<
      std::is_void<
    decltype(cb(const_cast<const MonMap&>(monmap),
		std::forward<Args>(args)...))>::value,
      void>::type {
    Mutex::Locker l(monc_lock);
    std::forward<Callback>(cb)(const_cast<const MonMap&>(monmap),
			       std::forward<Args>(args)...);
  }

private:
  struct version_req_d {
    Context *context;
    version_t *newest, *oldest;
    version_req_d(Context *con, version_t *n, version_t *o) : context(con),newest(n), oldest(o) {}
  };

  map<ceph_tid_t, version_req_d*> version_requests;
  ceph_tid_t version_req_id;
  void handle_get_version_reply(MMonGetVersionReply* m);


};

#endif
