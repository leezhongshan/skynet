/*
** $Id: lstring.c,v 2.49 2015/06/01 16:34:37 roberto Exp $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#define lstring_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"


#define MEMERRMSG       "not enough memory"


/*
** Lua will use at most ~(2^LUAI_HASHLIMIT) bytes from a string to
** compute its hash
*/
#if !defined(LUAI_HASHLIMIT)
#define LUAI_HASHLIMIT		5
#endif


/*
** equality for long strings
*/
int luaS_eqlngstr (TString *a, TString *b) {
  size_t len = a->u.lnglen;
  lua_assert(a->tt == LUA_TLNGSTR && b->tt == LUA_TLNGSTR);
  return (a == b) ||  /* same instance or... */
    ((len == b->u.lnglen) &&  /* equal length and ... */
     (memcmp(getstr(a), getstr(b), len) == 0));  /* equal contents */
}


unsigned int luaS_hash (const char *str, size_t l, unsigned int seed) {
  unsigned int h = seed ^ cast(unsigned int, l);
  size_t l1;
  size_t step = (l >> LUAI_HASHLIMIT) + 1;
  for (l1 = l; l1 >= step; l1 -= step)
    h = h ^ ((h<<5) + (h>>2) + cast_byte(str[l1 - 1]));
  return h;
}


/*
** resizes the string table
*/
void luaS_resize (lua_State *L, int newsize) {
  int i;
  stringtable *tb = &G(L)->strt;
  if (newsize > tb->size) {  /* grow table if needed */
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
    for (i = tb->size; i < newsize; i++)
      tb->hash[i] = NULL;
  }
  for (i = 0; i < tb->size; i++) {  /* rehash */
    TString *p = tb->hash[i];
    tb->hash[i] = NULL;
    while (p) {  /* for each node in the list */
      TString *hnext = p->u.hnext;  /* save next */
      unsigned int h = lmod(p->hash, newsize);  /* new position */
      p->u.hnext = tb->hash[h];  /* chain it */
      tb->hash[h] = p;
      p = hnext;
    }
  }
  if (newsize < tb->size) {  /* shrink table if needed */
    /* vanishing slice should be empty */
    lua_assert(tb->hash[newsize] == NULL && tb->hash[tb->size - 1] == NULL);
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
  }
  tb->size = newsize;
}


/*
** Clear API string cache. (Entries cannot be empty, so fill them with
** a non-collectable string.)
*/
void luaS_clearcache (global_State *g) {
  int i;
  for (i = 0; i < STRCACHE_SIZE; i++) {
    if (iswhite(g->strcache[i][0]))  /* will entry be collected? */
      g->strcache[i][0] = g->memerrmsg;  /* replace it with something fixed */
  }
}


/*
** Initialize the string table and the string cache
*/
void luaS_init (lua_State *L) {
  global_State *g = G(L);
  int i;
  luaS_resize(L, MINSTRTABSIZE);  /* initial size of string table */
  /* pre-create memory-error message */
  g->memerrmsg = luaS_newliteral(L, MEMERRMSG);
  luaC_fix(L, obj2gco(g->memerrmsg));  /* it should never be collected */
  for (i = 0; i < STRCACHE_SIZE; i++)  /* fill cache with valid strings */
    g->strcache[i][0] = g->memerrmsg;
}



/*
** creates a new string object
*/
static TString *createstrobj (lua_State *L, const char *str, size_t l,
                              int tag, unsigned int h) {
  TString *ts;
  GCObject *o;
  size_t totalsize;  /* total size of TString object */
  totalsize = sizelstring(l);
  o = luaC_newobj(L, tag, totalsize);
  ts = gco2ts(o);
  ts->hash = h;
  ts->extra = 0;
  memcpy(getaddrstr(ts), str, l * sizeof(char));
  getaddrstr(ts)[l] = '\0';  /* ending 0 */
  return ts;
}


void luaS_remove (lua_State *L, TString *ts) {
  stringtable *tb = &G(L)->strt;
  TString **p = &tb->hash[lmod(ts->hash, tb->size)];
  while (*p != ts)  /* find previous element */
    p = &(*p)->u.hnext;
  *p = (*p)->u.hnext;  /* remove element from its list */
  tb->nuse--;
}


/*
** checks whether short string exists and reuses it or creates a new one
*/
static TString *queryshrstr (lua_State *L, const char *str, size_t l, unsigned int h) {
  TString *ts;
  global_State *g = G(L);
  TString **list = &g->strt.hash[lmod(h, g->strt.size)];
  for (ts = *list; ts != NULL; ts = ts->u.hnext) {
    if (l == ts->shrlen &&
        (memcmp(str, getstr(ts), l * sizeof(char)) == 0)) {
      /* found! */
      if (isdead(g, ts))  /* dead (but not collected yet)? */
        changewhite(ts);  /* resurrect it */
      return ts;
    }
  }
  return NULL;
}

static TString *addshrstr (lua_State *L, const char *str, size_t l, unsigned int h) {
  TString *ts;
  global_State *g = G(L);
  TString **list = &g->strt.hash[lmod(h, g->strt.size)];
  if (g->strt.nuse >= g->strt.size && g->strt.size <= MAX_INT/2) {
    luaS_resize(L, g->strt.size * 2);
    list = &g->strt.hash[lmod(h, g->strt.size)];  /* recompute with new size */
  }
  ts = createstrobj(L, str, l, LUA_TSHRSTR, h);
  ts->shrlen = cast_byte(l);
  ts->u.hnext = *list;
  *list = ts;
  g->strt.nuse++;
  return ts;
}

static TString *internshrstr (lua_State *L, const char *str, size_t l);

/*
** new string (with explicit length)
*/
TString *luaS_newlstr (lua_State *L, const char *str, size_t l) {
  if (l <= LUAI_MAXSHORTLEN)  /* short string? */
    return internshrstr(L, str, l);
  else {
    TString *ts;
    if (l + 1 > (MAX_SIZE - sizeof(TString))/sizeof(char))
      luaM_toobig(L);
    ts = createstrobj(L, str, l, LUA_TLNGSTR, G(L)->seed);
    ts->u.lnglen = l;
    return ts;
  }
}


/*
** Create or reuse a zero-terminated string, first checking in the
** cache (using the string address as a key). The cache can contain
** only zero-terminated strings, so it is safe to use 'strcmp' to
** check hits.
*/
TString *luaS_new (lua_State *L, const char *str) {
  unsigned int i = point2uint(str) % STRCACHE_SIZE;  /* hash */
  TString **p = G(L)->strcache[i];
  if (strcmp(str, getstr(p[0])) == 0)  /* hit? */
    return p[0];  /* that it is */
  else {  /* normal route */
    TString *s = luaS_newlstr(L, str, strlen(str));
    p[0] = s;
    return s;
  }
}


Udata *luaS_newudata (lua_State *L, size_t s) {
  Udata *u;
  GCObject *o;
  if (s > MAX_SIZE - sizeof(Udata))
    luaM_toobig(L);
  o = luaC_newobj(L, LUA_TUSERDATA, sizeludata(s));
  u = gco2u(o);
  u->len = s;
  u->metatable = NULL;
  setuservalue(L, u, luaO_nilobject);
  return u;
}

/*
 * global shared table
 */

#include "rwlock.h"
#include "atomic.h"
#include <stdlib.h>

#define SHRSTR_SLOT 0x10000
#define HASH_NODE(h) ((h) % SHRSTR_SLOT)

struct shrmap_slot {
	struct rwlock lock;
	TString *str;
};

struct shrmap {
	struct shrmap_slot h[SHRSTR_SLOT];
	int n;
};

static struct shrmap *SSM = NULL;

LUA_API void
luaS_initshr() {
	struct shrmap * s = malloc(sizeof(*s));
	memset(s, 0, sizeof(*s));
	int i;
	for (i=0;i<SHRSTR_SLOT;i++) {
		rwlock_init(&s->h[i].lock);
	}
	SSM = s;
}

LUA_API void
luaS_exitshr() {
	int i;
	for (i=0;i<SHRSTR_SLOT;i++) {
		TString *str = SSM->h[i].str;
		while (str) {
			TString * next = str->u.hnext;
			free(str);
			str = next;
		}
	}
	free(SSM);
}

static TString *
query_string(unsigned int h, const char *str, lu_byte l) {
	struct shrmap_slot *s = &SSM->h[HASH_NODE(h)];
	rwlock_rlock(&s->lock);
	TString *ts = s->str;
	while (ts) {
		if (ts->hash == h && 
			ts->shrlen == l &&
			memcmp(str, ts+1, l) == 0) {
			break;
		}
		ts = ts->u.hnext;
	}
	rwlock_runlock(&s->lock);
	return ts;
}

static TString *
query_ptr(TString *t) {
	unsigned int h = t->hash;
	struct shrmap_slot *s = &SSM->h[HASH_NODE(h)];
	rwlock_rlock(&s->lock);
	TString *ts = s->str;
	while (ts) {
		if (ts == t)
			break;
		ts = ts->u.hnext;
	}
	rwlock_runlock(&s->lock);
	return ts;
}

static TString *
new_string(unsigned int h, const char *str, lu_byte l) {
	size_t sz = sizelstring(l);
	TString *ts = malloc(sz);
	memset(ts, 0, sz);
	ts->tt = LUA_TSHRSTR;
	ts->hash = h;
	ts->shrlen = l;
	memcpy(ts+1, str, l);
	return ts;
}

static TString *
add_string(unsigned int h, const char *str, lu_byte l) {
	TString * tmp = new_string(h, str, l);
	struct shrmap_slot *s = &SSM->h[HASH_NODE(h)];
	rwlock_wlock(&s->lock);
	TString *ts = s->str;
	while (ts) {
		if (ts->hash == h && 
			ts->shrlen == l &&
			memcmp(str, ts+1, l) == 0) {
				break;
		}
		ts = ts->u.hnext;
	}
	if (ts == NULL) {
		ts = tmp;
		ts->u.hnext = s->str;
		s->str = ts;
		tmp = NULL;
	}
	rwlock_wunlock(&s->lock);
	if (tmp) {
		// string is create by other thread, so free tmp
		free(tmp);
	}
	return ts;
}

static TString *
internshrstr (lua_State *L, const char *str, size_t l) {
  TString *ts;
  global_State *g = G(L);
  unsigned int h = luaS_hash(str, l, g->seed);
  unsigned int h0;
  // lookup global state of this L first
  ts = queryshrstr (L, str, l, h);
  if (ts)
    return ts;
  // lookup SSM again
  h0 = luaS_hash(str, l, 0);
  ts = query_string(h0, str, l);
  if (ts)
    return ts;
  // If SSM->n greate than 0, add it to SSM
  if (SSM->n > 0) {
    ATOM_DEC(&SSM->n);
    return add_string(h0, str, l);
  }
  // Else add it to global state (local)
  return addshrstr (L, str, l, h);
}

LUA_API void
luaS_expandshr(int n) {
  ATOM_ADD(&SSM->n, n);
}

LUAI_FUNC TString *
luaS_clonestring(lua_State *L, TString *ts) {
  unsigned int h;
  int l;
  const char * str = getaddrstr(ts);
  global_State *g = G(L);
  TString *result;
  if (ts->tt == LUA_TLNGSTR) 
    return luaS_newlstr(L, str, ts->u.lnglen);
  // look up global state of this L first
  l = ts->shrlen;
  h = luaS_hash(str, l, g->seed);
  result = queryshrstr (L, str, l, h);
  if (result)
    return result;
  // look up SSM by ptr
  result = query_ptr(ts);
  if (result)
    return result;
  // ts is not in SSM, so recalc hash, and add it to SSM
  h = luaS_hash(str, l, 0);
  return add_string(h, str, l);
}

struct slotinfo {
	int len;
	int size;
};

static void
getslot(struct shrmap_slot *s, struct slotinfo *info) {
	memset(info, 0, sizeof(*info));
	rwlock_rlock(&s->lock);
	TString *ts = s->str;
	while (ts) {
		++info->len;
		info->size += sizelstring(ts->shrlen);
		ts = ts->u.hnext;
	}
	rwlock_runlock(&s->lock);
}

LUA_API int
luaS_shrinfo(lua_State *L) {
	struct slotinfo total;
	struct slotinfo tmp;
	memset(&total, 0, sizeof(total));
	int i;
	int len = 0;
	for (i=0;i<SHRSTR_SLOT;i++) {
		struct shrmap_slot *s = &SSM->h[i];
		getslot(s, &tmp);
		len += tmp.len;
		if (tmp.len > total.len) {
			total.len = tmp.len;
		}
		total.size += tmp.size;
	}
	lua_pushinteger(L, len);
	lua_pushinteger(L, total.size);
	lua_pushinteger(L, total.len);
	lua_pushinteger(L, SSM->n);
	return 4;
}
