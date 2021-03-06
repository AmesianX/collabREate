/*
   collabREate cli_mgr.cpp
   Copyright (C) 2018 Chris Eagle <cseagle at gmail d0t com>
   Copyright (C) 2018 Tim Vidas <tvidas at gmail d0t com>

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2 of the License, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
   more details.

   You should have received a copy of the GNU General Public License along with
   this program; if not, write to the Free Software Foundation, Inc., 59 Temple
   Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "utils.h"
#include "client.h"
#include "proj_info.h"
#include "cli_mgr.h"
#include "projectmap.h"
#include "clientset.h"
#include "io.h"

UserInfo::UserInfo(const char *uname, uint32_t _uid, uint64_t _pub, uint64_t _sub) : username(uname) {
   uid = _uid;
   pub = _pub;
   sub = _sub;
}

UserInfo::UserInfo() : username("invalid") {
   uid = INVALID_UID;
   pub = 0;
   sub = 0;
}

Packet::Packet(Client *src, const char *cmd, json_object *obj, uint64_t updateid) {
   c = src;
   this->cmd = cmd;
   this->obj = obj;
   uid = updateid;
   append_json_uint64_val(obj, "updateid", updateid);   //is this really necessary?
}

/**
 * For use in Basic mode when a Global project ID is not needed
 */
const char * const ConnectionManager::EMPTY_GPID = "0000000000000000000000000000000000000000000000000000000000000000";

ConnectionManager::ConnectionManager(json_object *conf) {
   this->conf = conf;
   done = false;
   sem_init(&pidLock, 0, 1);
   sem_init(&queueSem, 0, 0);
   sem_init(&queueMutex, 0, 1);
}

const UserInfo &ConnectionManager::getUserInfo(uint32_t uid) {
   static UserInfo invalid;
   if (user_map.find(uid) == user_map.end()) {
      return invalid;
   }
   return user_map[uid];
}

void ConnectionManager::start() {
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   pthread_t tid;
   pthread_create(&tid, &attr, run, (void*)this);
}

static bool termClients(Client *c, void *user) {
   c->terminate();
   return true;
}

/**
 * terminate terminates the connection manager
 * it terminates all clients connected to all projects
 */
void ConnectionManager::terminate() {
   log(LINFO, "ConnectionManager terminating\n");
   done = true;
   projects.loopClients(termClients, NULL);
   if (conf != NULL) {
      json_object_put(conf);
      conf = NULL;
   }
}

/**
 * remove removes a client from a currently reflecting project
 * @param c the client to remove (from whatever project it is already connected to)
 */
void ConnectionManager::remove(Client *c) {
//  logln("Removing client from " + c->getGpid() + " chain", LINFO1);
   projects.removeClient(c);
}

static bool clientStats(Client *c, void *user) {
   string *s = (string*)user;
   *s += c->dumpStats();
   return true;
}

/**
 * dumpStats dumps send / receive stats for each connected client
 */
string ConnectionManager::dumpStats() {
   string sb = "";
   projects.loopClients(clientStats, &sb);
   if (sb.length() == 0) {
      sb = "Stats:\n - none - \n";
   }
   else {
      sb = "Stats:\n" + sb;
   }
   return sb;
}

static bool dispatch(Client *c, void *user) {
   Packet *p = (Packet*)user;

   if (c != p->c) {  //only send to other than originator
      //increment ref count on json object before sending
      //because writeJson will decrement it and we can't have the object
      //garbage collected until all clients have received it
      json_object_get(p->obj);
      c->post(p->cmd, p->obj);
   }
   else {
      //send updateid back to the originator
      json_object *obj = json_object_new_object();
      append_json_uint64_val(obj, "updateid", p->uid);
      c->send_data(MSG_ACK_UPDATEID, obj);
   }

   return true;
}

/**
 * run perpetually waits to be notified that a new packet has been queued, then
 * sends this packet to other clients according to permissions and project subscription
 * this also sends the server created unique updateID back to the originator of the packet
 */
void *ConnectionManager::run(void *arg) {
   ConnectionManager *mgr = (ConnectionManager*)arg;
   while (!mgr->done) {
      sem_wait(&mgr->queueSem);
      sem_wait(&mgr->queueMutex);
      Packet *p = mgr->queue[0];
      //*** does add/remove need to be synchronized on vectors?
      mgr->queue.erase(mgr->queue.begin());
      sem_post(&mgr->queueMutex);
      //get the project associated with this notification
      mgr->projects.loopProject(p->c->getPid(), dispatch, p);
      json_object_put(p->obj);
      delete p;
   }
   return NULL;
}

static bool clientList(Client *c, void *user) {
   string *s = (string*)user;
   char buf[64];
   int len;
   //logln(cnt + c.getPeerAddr() + ":" + c.getPort() + "  (?|" + c.getPub() + ")    (?|" + c.getSub() + ")  ????");
   snprintf(buf, sizeof(buf), "%-9d", c->getUid());
   *s += buf;
   len = snprintf(buf, sizeof(buf), "%s:", c->getPeerAddr().c_str());
   *s += buf;
   snprintf(buf, sizeof(buf), "%-*d", 30 - len, c->getPeerPort());
   *s += buf;
   snprintf(buf, sizeof(buf), "0x%08x     ", (uint32_t)c->getPub());
   *s += buf;
   snprintf(buf, sizeof(buf), "0x%08x     ", (uint32_t)c->getSub());
   *s += buf;
   snprintf(buf, sizeof(buf), "%-5d ", c->getPid());
   *s += buf;
   snprintf(buf, sizeof(buf), "%3d: %s \n", c->getUid(), c->getUser().c_str());
   *s += buf;
   return true;
}

/**
 * listConnection displays the current connections to the collabREate connection manager
 */
string ConnectionManager::listConnections() {
   string sb = "";
   projects.loopClients(clientList, &sb);
   if (sb.length() == 0) {
      sb = "Client   Address:Port                  Pub(Effective) Sub(Effective) PID     User\n - none - \n";
   }
   else {
      sb = "Client   Address:Port                  Pub(Effective) Sub(Effective) PID     User\n" + sb;
   }
   return sb;
}

