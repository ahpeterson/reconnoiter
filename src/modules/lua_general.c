/*
 * Copyright (c) 2013, Circonus, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name Circonus, Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "noit_defines.h"
#include "noit_module.h"
#include "noit_http.h"
#include "noit_rest.h"
#include "noit_filters.h"
#include "noit_reverse_socket.h"
#define LUA_COMPAT_MODULE
#include "lua_noit.h"
#include "lua_http.h"
#include <assert.h>

static noit_log_stream_t nlerr = NULL;
static noit_log_stream_t nldeb = NULL;

#define lua_general_xml_description  ""
static const char *script_dir = "";

typedef struct lua_general_conf {
  lua_module_closure_t lmc;
  const char *script_dir;
  const char *module;
  const char *function;
  lua_State *L;
} lua_general_conf_t;

static lua_general_conf_t *get_config(noit_module_generic_t *self) {
  lua_general_conf_t *conf = noit_image_get_userdata(&self->hdr);
  if(conf) return conf;
  conf = calloc(1, sizeof(*conf));
  noit_image_set_userdata(&self->hdr, conf);
  return conf;
}

static void
lua_general_ctx_free(void *cl) {
  noit_lua_resume_info_t *ri = cl;
  if(ri) {
    noitL(nldeb, "lua_general(%p) -> stopping job (%p)\n",
          ri->lmc->lua_state, ri->coro_state);
    noit_lua_cancel_coro(ri);
    noit_lua_resume_clean_events(ri);
    free(ri);
  }
}

static int
lua_general_resume(noit_lua_resume_info_t *ri, int nargs) {
  const char *err = NULL;
  int status, base, rv = 0;

#if LUA_VERSION_NUM >= 502
  status = lua_resume(ri->coro_state, ri->lmc->lua_state, nargs);
#else
  status = lua_resume(ri->coro_state, nargs);
#endif

  switch(status) {
    case 0: break;
    case LUA_YIELD:
      lua_gc(ri->coro_state, LUA_GCCOLLECT, 0);
      return 0;
    default: /* The complicated case */
      base = lua_gettop(ri->coro_state);
      if(base>=0) {
        if(lua_isstring(ri->coro_state, base-1)) {
          err = lua_tostring(ri->coro_state, base-1);
          noitL(nlerr, "err -> %s\n", err);
        }
      }
      rv = -1;
  }

  lua_general_ctx_free(ri);
  return rv;
}

static noit_lua_resume_info_t *
lua_general_new_resume_info(lua_module_closure_t *lmc) {
  noit_lua_resume_info_t *ri;
  ri = calloc(1, sizeof(*ri));
  ri->context_magic = LUA_GENERAL_INFO_MAGIC;
  ri->lmc = lmc;
  lua_getglobal(lmc->lua_state, "noit_coros");
  ri->coro_state = lua_newthread(lmc->lua_state);
  ri->coro_state_ref = luaL_ref(lmc->lua_state, -2);
  noit_lua_set_resume_info(lmc->lua_state, ri);
  lua_pop(lmc->lua_state, 1); /* pops noit_coros */
  noitL(nldeb, "lua_general(%p) -> starting new job (%p)\n",
        lmc->lua_state, ri->coro_state);
  return ri;
}

static int
lua_general_handler(noit_module_generic_t *self) {
  int status, rv;
  lua_general_conf_t *conf = get_config(self);
  lua_module_closure_t *lmc = &conf->lmc;
  noit_lua_resume_info_t *ri = NULL;;
  const char *err = NULL;
  char errbuf[128];
  lua_State *L;

  if(!lmc || !conf || !conf->module || !conf->function) {
    goto boom;
  }
  ri = lua_general_new_resume_info(lmc);
  L = ri->coro_state;

  lua_getglobal(L, "require");
  lua_pushstring(L, conf->module);
  rv = lua_pcall(L, 1, 1, 0);
  if(rv) {
    int i;
    noitL(nlerr, "lua: require %s failed\n", conf->module);
    i = lua_gettop(L);
    if(i>0) {
      if(lua_isstring(L, i)) {
        const char *err;
        size_t len;
        err = lua_tolstring(L, i, &len);
        noitL(nlerr, "lua: %s\n", err);
      }
    }
    lua_pop(L,i);
    goto boom;
  }
  lua_pop(L, lua_gettop(L));

  noit_lua_pushmodule(L, conf->module);
  if(lua_isnil(L, -1)) {
    lua_pop(L, 1);
    snprintf(errbuf, sizeof(errbuf), "no such module: '%s'", conf->module);
    err = errbuf;
    goto boom;
  }
  lua_getfield(L, -1, conf->function);
  lua_remove(L, -2);
  if(!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    snprintf(errbuf, sizeof(errbuf), "no function '%s' in module '%s'",
             conf->function, conf->module);
    err = errbuf;
    goto boom;
  }

  status = lmc->resume(ri, 0);
  if(status == 0) return 0;
  /* If we've failed, resume has freed ri, so we should just return. */
  noitL(nlerr, "lua dispatch error: %d\n", status);
  return 0;

 boom:
  if(err) noitL(nlerr, "lua dispatch error: %s\n", err);
  if(ri) lua_general_ctx_free(ri);
  return 0;
}

static int
lua_general_coroutine_spawn(lua_State *Lp) {
  int nargs;
  lua_State *L;
  noit_lua_resume_info_t *ri_parent = NULL, *ri = NULL;

  nargs = lua_gettop(Lp);
  if(nargs < 1 || !lua_isfunction(Lp,1))
    luaL_error(Lp, "noit.coroutine_spawn(func, ...): bad arguments");
  ri_parent = noit_lua_get_resume_info(Lp);
  assert(ri_parent);

  ri = lua_general_new_resume_info(ri_parent->lmc);
  L = ri->coro_state;
  lua_xmove(Lp, L, nargs);
#if !defined(LUA_JITLIBNAME) && LUA_VERSION_NUM < 502
  lua_setlevel(Lp, L);
#endif
  ri->lmc->resume(ri, nargs-1);
  return 0;
}

int
dispatch_general(eventer_t e, int mask, void *cl, struct timeval *now) {
  return lua_general_handler((noit_module_generic_t *)cl);
}

static int
noit_lua_general_config(noit_module_generic_t *self, noit_hash_table *o) {
  lua_general_conf_t *conf = get_config(self);
  conf->script_dir = "";
  conf->module = NULL;
  conf->function = NULL;
  (void)noit_hash_retr_str(o, "directory", strlen("directory"), &conf->script_dir);
  if(conf->script_dir) conf->script_dir = strdup(conf->script_dir);
  (void)noit_hash_retr_str(o, "lua_module", strlen("lua_module"), &conf->module);
  if(conf->module) conf->module = strdup(conf->module);
  (void)noit_hash_retr_str(o, "lua_function", strlen("lua_function"), &conf->function);
  if(conf->function) conf->function = strdup(conf->function);
  return 0;
}

static int
lua_general_reverse_socket_initiate(lua_State *L) {
  const char *host;
  int port;
  noit_hash_table *sslconfig = NULL, *config = NULL;
  if(lua_gettop(L) < 2 ||
     !lua_isstring(L,1) ||
     !lua_isnumber(L,2) ||
     (lua_gettop(L) >= 3 && !lua_istable(L,3)) ||
     (lua_gettop(L) >= 4 && !lua_istable(L,4)))
    luaL_error(L, "reverse_start(host,port,sslconfig,config)");

  host = lua_tostring(L,1);
  port = lua_tointeger(L,2);
  if(lua_gettop(L)>=3) sslconfig = noit_lua_table_to_hash(L,3);
  if(lua_gettop(L)>=4) config = noit_lua_table_to_hash(L,4);

  noit_lua_help_initiate_noit_connection(host, port, sslconfig, config);

  if(sslconfig) {
    noit_hash_destroy(sslconfig, NULL, NULL);
    free(sslconfig);
  }
  if(config) {
    noit_hash_destroy(config, NULL, NULL);
    free(config);
  }
  return 0;
}
static int
lua_general_reverse_socket_shutdown(lua_State *L) {
  int rv;
  if(lua_gettop(L) < 2 ||
     !lua_isstring(L,1) ||
     !lua_isnumber(L,2))
    luaL_error(L, "reverse_stop(host,port)");

  rv = noit_reverse_socket_connection_shutdown(lua_tostring(L,1), lua_tointeger(L,2));
  lua_pushboolean(L,rv);
  return 1;
}

static int
lua_general_filtersets_cull(lua_State *L) {
  int rv;
  rv = noit_filtersets_cull_unused();
  if(rv > 0) noit_conf_mark_changed();
  lua_pushinteger(L, rv);
  return 1;
}

static int
lua_general_conf_save(lua_State *L) {
  /* Invert the response to indicate a truthy success in lua */
  lua_pushboolean(L, noit_conf_write_file(NULL) ? 0 : 1);
  return 1;
}

static const luaL_Reg general_lua_funcs[] =
{
  {"coroutine_spawn", lua_general_coroutine_spawn },
  {"reverse_start", lua_general_reverse_socket_initiate },
  {"reverse_stop", lua_general_reverse_socket_shutdown },
  {"filtersets_cull", lua_general_filtersets_cull },
  {"conf_save", lua_general_conf_save },
  {NULL,  NULL}
};


static int
noit_lua_general_init(noit_module_generic_t *self) {
  lua_general_conf_t *conf = get_config(self);
  lua_module_closure_t *lmc = &conf->lmc;

  if(!conf->module || !conf->function) {
    noitL(nlerr, "lua_general cannot be used without module and function config\n");
    return -1;
  }

  lmc->resume = lua_general_resume;
  lmc->lua_state = noit_lua_open(self->hdr.name, lmc, conf->script_dir);
  noitL(nldeb, "lua_general opening state -> %p\n", lmc->lua_state);
  if(lmc->lua_state == NULL) {
    noitL(noit_error, "lua_general could not add general functions\n");
    return -1;
  }
  luaL_openlib(lmc->lua_state, "noit", general_lua_funcs, 0);
  lmc->pending = calloc(1, sizeof(*lmc->pending));
  eventer_add_in_s_us(dispatch_general, self, 0, 0);
  return 0;
}

static int
noit_lua_general_onload(noit_image_t *self) {
  nlerr = noit_log_stream_find("error/lua");
  nldeb = noit_log_stream_find("debug/lua");
  if(!nlerr) nlerr = noit_stderr;
  if(!nldeb) nldeb = noit_debug;
  return 0;
}

noit_module_generic_t lua_general = {
  {
    NOIT_GENERIC_MAGIC,
    NOIT_GENERIC_ABI_VERSION,
    "lua_general",
    "general services in lua",
    lua_general_xml_description,
    noit_lua_general_onload
  },
  noit_lua_general_config,
  noit_lua_general_init
};
