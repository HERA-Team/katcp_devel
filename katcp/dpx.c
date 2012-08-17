#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <netc.h>
#include <katcp.h>
#include <katpriv.h>
#include <katcl.h>

#define FLAT_MAGIC 0x49021faf

#define FLAT_STATE_GONE   0
#define FLAT_STATE_NEW    1
#define FLAT_STATE_UP     2
#define FLAT_STATE_DRAIN  3

/* was supposed to be called duplex, but flat is punnier */
/********************************************************************/

struct katcp_flat{
  unsigned int f_magic;
  char *f_name;          /* locate the thing by name */

  int f_flags;           /* which directions can we do */

  int f_state;           /* up, shutting down, etc */
  int f_exit_code;       /* reported exit status */

  int f_log_level;       /* log level currently set */

  struct katcp_notice *f_halt;

  struct katcl_line *f_line;
  struct katcp_shared *f_shared;

  struct katcl_parse *f_rx;      /* received message */

  struct katcl_queue *f_backlog; /* backed up requests from the remote end, shouldn't happen */
};

struct katcp_cmd_item{
  char *i_name;
  char *i_help;
  int (*i_call)(struct katcp_dispatch *d, int argc);
  unsigned int i_flags;
  char *i_data;
  void (*i_clear)(void *data);
};

#include <avltree.h>

struct katcp_cmd_map{
  char *m_name;
  unsigned int m_refs;
  struct avl_tree *m_tree;
  struct katcp_cmd_item *m_fallback;
};


void destroy_cmd_item(struct katcp_cmd_item *i)
{
  if(i == NULL){
    return;
  }

  if(i->i_name){
    free(i->i_name);
    i->i_name = NULL;
  }

  if(i->i_help){
    free(i->i_help);
    i->i_help = NULL;
  }

  i->i_call = NULL;
  i->i_flags = 0;

  if(i->i_clear){
    (*(i->i_clear))(i->i_data);
    i->i_clear = NULL;
  }

  i->i_data = NULL;

  free(i);
}

void destroy_cmd_item_void(void *v)
{
  struct katcp_cmd_item *i;

  i = v;

  destroy_cmd_item(i);
}
struct katcp_cmd_item *create_cmd_item(char *name, char *help, unsigned int flags, int (*call)(struct katcp_dispatch *d, int argc), void *data, void (*clear)(void *data)){
  struct katcp_cmd_item *i;

  i = malloc(sizeof(struct katcp_cmd_item));
  if(i == NULL){
    return NULL;
  }

  i->i_name = NULL;
  i->i_help = NULL;

  i->i_flags = 0;

  i->i_call = NULL;
  i->i_data = NULL;
  i->i_clear = NULL;

  if(i->i_name){
    i->i_name = strdup(name);
    if(i->i_name == NULL){
      destroy_cmd_item(i);
      return NULL;
    }
  }

  if(i->i_help){
    i->i_help = strdup(help);
    if(i->i_help == NULL){
      destroy_cmd_item(i);
      return NULL;
    }
  }

  i->i_flags = flags;

  i->i_call = call;
  i->i_data = data;
  i->i_clear = clear;

  return i;
}

void destroy_cmd_map(struct katcp_cmd_map *m)
{
  if(m == NULL){
    return;
  }

  if(m->m_refs > 0){
    m->m_refs--;
  }

  if(m->m_refs > 0){
    return;
  }

  if(m->m_tree){
    destroy_avltree(m->m_tree, &destroy_cmd_item_void);
    m->m_tree = NULL;
  }

  if(m->m_fallback){
    destroy_cmd_item(m->m_fallback);
    m->m_fallback = NULL;
  }

  if(m->m_name){
    free(m->m_name);
    m->m_name = NULL;
  }
}

struct katcp_cmd_map *create_cmd_map(char *name)
{
  struct katcp_cmd_map *m;

  m = malloc(sizeof(struct katcp_cmd_map));
  if(m == NULL){
    return NULL;
  }

  m->m_name = NULL;
  m->m_refs = 0;
  m->m_tree = NULL;
  m->m_fallback = NULL;

  m->m_tree = create_avltree();
  if(m->m_tree == NULL){
    destroy_cmd_map(m);
    return NULL;
  }

  if(name){
    m->m_name = strdup(name);
    if(m->m_name == NULL){
      destroy_cmd_map(m);
      return NULL;
    }
  }

  return m;
}

int add_full_cmd_map(struct katcp_cmd_map *m, char *name, char *help, unsigned int flags, int (*call)(struct katcp_dispatch *d, int argc), void *data, void (*clear)(void *data))
{
  struct katcp_cmd_item *i;
  struct avl_node *n;

  if(name == NULL){
    return -1;
  }

  switch(name[0]){
    case KATCP_REQUEST : 
    case KATCP_REPLY :
    case KATCP_INFORM : 
      break;
    default: 
      return -1;
  }

  if(name[1] == '\0'){
    return -1;
  }

  i = create_cmd_item(name, help, flags, call, data, clear);
  if(i == NULL){
    return -1;
  }

  n = create_node_avltree(name + 1, i);
  if(n == NULL){
    destroy_cmd_item(i);
    return -1;
  }
  
  if(add_node_avltree(m->m_tree, n) < 0){
    free_node_avltree(n, &destroy_cmd_item_void);
    return -1;
  }

  return 0;
}

int add_cmd_map(struct katcp_cmd_map *m, char *name, char *help, int (*call)(struct katcp_dispatch *d, int argc))
{
  return add_full_cmd_map(m, name, help, 0, call, NULL, NULL);
}

struct katcp_cmd_item *locate_cmd_item(struct katcp_flat *f, struct katcl_parse *p)
{
  struct katcp_cmd_item *i;
  struct katcp_cmd_map *m;
  char *str;

  str = get_string_parse_katcl(p, 0);
  if(str == NULL){
    return NULL;
  }

  if(str[0] == '\0'){
    return NULL;
  }

#if 1
  /* map not initialised */
  return NULL;
#endif

  i = find_data_avltree(m->m_tree, str + 1);
  if(i){
    return i;
  }

  return m->m_fallback;
}


/********************************************************************/

/* */

#ifdef DEBUG
static void sane_flat_katcp(struct katcp_flat *f)
{
  if(f->f_magic != FLAT_MAGIC){
    fprintf(stderr, "flat: bad magic 0x%x, expected 0x%x\n", f->f_magic, FLAT_MAGIC);
    abort();
  }
}
#else 
#define sane_flat_katcp(f);
#endif

static void destroy_flat_katcp(struct katcp_dispatch *d, struct katcp_flat *f)
{
  sane_flat_katcp(f);

  if(f->f_line){
    destroy_katcl(f->f_line, 1);
    f->f_line = NULL;
  }

  if(f->f_name){
    free(f->f_name);
    f->f_name = NULL;
  }

  if(f->f_halt){
    /* TODO: wake halt notice watching us */

    f->f_halt = NULL;
  }

  if(f->f_backlog){
    destroy_queue_katcl(f->f_backlog);
    f->f_backlog = NULL;
  }


  if(f->f_rx){
#if 0
    /* destroy currently not needed, operate on assumption that it is transient ? */
    destroy_parse_katcl(f->f_rx);
#endif
    f->f_rx = NULL;
  }

  /* will probably have to decouple ourselves here */
  f->f_shared = NULL;
  f->f_magic = 0;

  free(f);
}

struct katcp_flat *create_flat_katcp(struct katcp_dispatch *d, int fd, char *name)
{
  /* TODO: what about cloning an existing one to preserve misc settings, including log level, etc */
  struct katcp_flat *f, **tmp;
  struct katcp_shared *s;

  s = d->d_shared;

  /* TODO: check if we have hit the ceiling */

  tmp = realloc(s->s_flats, sizeof(struct katcp_flat *) * (s->s_floors + 1));
  if(tmp == NULL){
    return NULL;
  }

  s->s_flats = tmp;

  f = malloc(sizeof(struct katcp_flat));
  if(f == NULL){
    return NULL;
  }

  f->f_magic = FLAT_MAGIC;
  f->f_name = NULL;

  f->f_flags = 0;

  f->f_state = FLAT_STATE_NEW;
  f->f_exit_code = 0; /* WARNING: should technically be a fail, in case we don't set it ourselves */

  f->f_log_level = s->s_default;

  f->f_halt = NULL;
  f->f_line = NULL;
  f->f_shared = NULL;

  f->f_backlog = NULL;
  f->f_rx = NULL;

  if(name){
    f->f_name = strdup(name);
    if(f->f_name == NULL){
      destroy_flat_katcp(d, f);
      return NULL;
    }
  }

  f->f_line = create_katcl(fd);
  if(f->f_line == NULL){
    destroy_flat_katcp(d, f);
    return NULL;
  }

  f->f_backlog = create_queue_katcl();
  if(f->f_backlog == NULL){
    destroy_flat_katcp(d, f);
    return NULL;
  }

  f->f_shared = s;

  s->s_flats[s->s_floors] = f;
  s->s_floors++;

  return f;
}

int load_flat_katcp(struct katcp_dispatch *d)
{
  struct katcp_flat *f;
  struct katcp_shared *s;
  unsigned int i;
  int result, fd;

  sane_flat_katcp(f);

  s = d->d_shared;

  result = 0;

#ifdef DEBUG
  fprintf(stderr, "flat: loading %u units\n", s->s_floors);
#endif

  for(i = 0; i < s->s_floors; i++){

    f = s->s_flats[i];
    fd = fileno_katcl(f->f_line);

    switch(f->f_state){
      case FLAT_STATE_GONE :
#ifdef DEBUG
        fprintf(stderr, "flat: problem: state %u should have been removed already\n", f->f_state);
        abort();
#endif
        break;

      case FLAT_STATE_UP : 
        FD_SET(fd, &(s->s_read));
        if(fd > s->s_max){
          s->s_max = fd;
        }
        break;

    }

    if(flushing_katcl(f->f_line)){
      FD_SET(fd, &(s->s_write));
      if(fd > s->s_max){
        s->s_max = fd;
      }
    }
    /* TODO */
  }

  return result;
}

int run_flat_katcp(struct katcp_dispatch *d)
{
  struct katcp_flat *f;
  struct katcp_shared *s;
  unsigned int i;
  int fd, result, r;
  char *str;

  sane_flat_katcp(f);

  s = d->d_shared;

  result = 0;

#ifdef DEBUG
  fprintf(stderr, "flat: running %u units\n", s->s_floors);
#endif

  for(i = 0; i < s->s_floors; i++){
    f = s->s_flats[i];

    s->s_this = f;

    fd = fileno_katcl(f->f_line);

    if(FD_ISSET(fd, &(s->s_write))){
      if(write_katcl(f->f_line) < 0){
        /* FAIL somehow */
      }
    }

    if(FD_ISSET(fd, &(s->s_read))){
      if(read_katcl(f->f_line) < 0){
        /* FAIL somehow */
      }

      while((r = parse_katcl(f->f_line)) > 0){
        f->f_rx = ready_katcl(f->f_line);
#if 1
        if(f->f_rx == NULL){
          log_message_katcp(d, KATCP_LEVEL_FATAL, NULL, "parse_katcl promised us data, but there isn't any");
          return -1;
        }
#endif

        str = get_string_parse_katcl(f->f_rx, 0);
        if(str){
          switch(str[0]){
            case KATCP_REQUEST :
              break;
            case KATCP_REPLY   :
              break;
            case KATCP_INFORM  :
              break;
            default :
              /* ignore malformed messages silently */
              break;
          }
        } /* else silently ignore */


      }

      if(r < 0){
        /* FAIL */
      }
    }


    /* TODO */
  }

  s->s_this = NULL;

  return result;
}

/****************************************************************/

int accept_flat_katcp(struct katcp_dispatch *d, struct katcp_arb *a, unsigned int mode)
{
#define LABEL_BUFFER 32
  int fd, nfd;
  unsigned int len;
  struct sockaddr_in sa;
  struct katcp_flat *f;
  char label[LABEL_BUFFER];
  long opts;

  fd = fileno_arb_katcp(d, a);

  if(!(mode & KATCP_ARB_READ)){
    log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "ay caramba, accept handler expected to be called with the read flag");
    return 0;
  }

  len = sizeof(struct sockaddr_in);
  nfd = accept(fd, (struct sockaddr *) &sa, &len);

  if(nfd < 0){
    log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "accept on %s failed: %s", name_arb_katcp(d, a), strerror(errno));
    return 0;
  }

  opts = fcntl(nfd, F_GETFL, NULL);
  if(opts >= 0){
    opts = fcntl(nfd, F_SETFL, opts | O_NONBLOCK);
  }

  snprintf(label, LABEL_BUFFER, "%s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
  label[LABEL_BUFFER - 1] = '\0';

  log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "accepted new connection from %s via %s", label, name_arb_katcp(d, a));

  f = create_flat_katcp(d, nfd, label);
  if(f == NULL){
    close(nfd);
  }
  
  return 0;
#undef LABEL_BUFFER
}

struct katcp_arb *listen_flat_katcp(struct katcp_dispatch *d, char *name)
{
  int fd;
  struct katcp_arb *a;
  long opts;

  fd = net_listen(name, 0, 0);
  if(fd < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to listen on %s: %s", name, strerror(errno));
    return NULL;
  }

  opts = fcntl(fd, F_GETFL, NULL);
  if(opts >= 0){
    opts = fcntl(fd, F_SETFL, opts | O_NONBLOCK);
  }

  a = create_arb_katcp(d, name, fd, KATCP_ARB_READ, &accept_flat_katcp, NULL);
  if(a == NULL){
    close(fd);
    return NULL;
  }

  return a;
}

int listen_duplex_cmd_katcp(struct katcp_dispatch *d, int argc)
{
  char *name;

  name = arg_string_katcp(d, 1);
  if(name == NULL){
    return extra_response_katcp(d, KATCP_RESULT_INVALID, "usage");
  }

  if(listen_flat_katcp(d, name) == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to listen on %s: %s", name, strerror(errno));
    return KATCP_RESULT_FAIL;
  }

  return KATCP_RESULT_OK;
}

int list_duplex_cmd_katcp(struct katcp_dispatch *d, int argc)
{
  unsigned int i;
  struct katcp_shared *s;

  struct katcp_flat *fx;

  s = d->d_shared;

  for(i = 0; i < s->s_floors; i++){
    fx = s->s_flats[i];
    log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "%s at %p in state %u", fx->f_name, fx, fx->f_state);
    log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "%s has queue backlog of %u", size_queue_katcl(fx->f_backlog));
  }

  return KATCP_RESULT_OK;
}
