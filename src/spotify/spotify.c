/*
 *  Backend using Spotify
 *  Copyright (C) 2009, 2010 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>

#include "showtime.h"
#include "navigator.h"
#include "playqueue.h"
#include "media.h"
#include "notifications.h"
#include "api.h"
#include "keyring.h"
#include "misc/ptrvec.h"
#include "sd/sd.h"
#include "scrappers/scrappers.h"
#include "misc/pixmap.h"

#ifdef CONFIG_LIBSPOTIFY_LOAD_RUNTIME
#include <dlfcn.h>
#endif
#include "apifunctions.h"
#include "spotify_app_key.h"

/**
 *
 */
static media_pipe_t *spotify_mp;
static hts_mutex_t spotify_mutex;
static hts_cond_t spotify_cond_main;
static hts_cond_t spotify_cond_login;
static int spotify_started;

static int play_position;
static int seek_pos;
static int is_logged_in;

static prop_t *prop_status;
static prop_t *prop_syncing_playlists;

static sp_session *spotify_session;
TAILQ_HEAD(spotify_msg_queue, spotify_msg);
static struct spotify_msg_queue spotify_msgs;

typedef enum {
  METADATA_TRACK,
  METADATA_ALBUM_NAME,
  METADATA_ALBUM_YEAR,
  METADATA_ALBUM_ARTIST_NAME,
  METADATA_ALBUM_IMAGE,
  METADATA_ARTIST_NAME,
} metadata_type_t;


/**
 * Binds a property to metadata in spotify
 * This will make sure the properties stays updated with the
 * spotify version of the track.
 * The structure self-destructs when the property tree is deleted (by
 * a subscription that checks PROP_DESTROYED)
 */
typedef struct metadata {
  LIST_ENTRY(metadata) m_link;
  prop_t *m_prop;
  struct playlist_track *m_plt;
  void *m_source;
  metadata_type_t m_type;
  int m_flags;
#define METADATA_ARTIST_IMAGES_SCRAPPED 0x1

} metadata_t;

static LIST_HEAD(, metadata) metadatas;
static hts_mutex_t meta_mutex;

/**
 * Playlist support
 *
 *
 */
prop_t *prop_playlists;

static ptrvec_t playlists;


typedef struct playlist {
  
  sp_playlist *pl_playlist;

  char *pl_url;
  ptrvec_t pl_tracks;
  int pl_position;
  prop_t *pl_prop_root;
  prop_t *pl_prop_tracks;
  prop_t *pl_prop_title;
  prop_t *pl_prop_type;
  prop_t *pl_prop_num_tracks;

} playlist_t;


typedef struct playlist_track {
  sp_track *plt_track;

  prop_t *plt_prop_root;
  prop_t *plt_prop_metadata;
  prop_t *plt_prop_title;

} playlist_track_t;

static void load_initial_playlists(sp_playlistcontainer *pc);


typedef enum {
  SPOTIFY_PENDING_EVENT, //< event pending from libspotify
  SPOTIFY_LOGOUT,
  SPOTIFY_PLAY_TRACK,
  SPOTIFY_LIST,
  SPOTIFY_STOP_PLAYBACK,
  SPOTIFY_RELEASE_TRACK,
  SPOTIFY_RELEASE_ALBUM,
  SPOTIFY_RELEASE_ARTIST,
  SPOTIFY_RELEASE_IMAGE,
  SPOTIFY_SEEK,
  SPOTIFY_PAUSE,
  SPOTIFY_GET_IMAGE,
  SPOTIFY_GET_PARENT,
  SPOTIFY_OPEN_TRACK,
  SPOTIFY_OPEN_PAGE,
} spotify_msg_type_t;

/**
 * A spotify URI
 */
typedef struct spotify_uri {
  const char *su_uri;

  char *su_errbuf;
  size_t su_errlen;
  int su_errcode;

  prop_t *su_metadata;
  prop_t *su_list;     // for be_spotify_list()

  prop_t *su_playlist_title;

  sp_link *su_playlist_link;

  sp_track *su_track;

  int su_artist_albums;

  sp_albumtype su_album_type;

} spotify_uri_t;

static hts_cond_t spotify_cond_uri;
static spotify_uri_t *su_playing, *su_pending;

/**
 * A spotify page query
 */
typedef struct spotify_page {
  prop_t *sp_root;
  char *sp_url;

} spotify_page_t;

/**
 * A spotify play reuqest
 */
typedef struct spotify_open_track {
  char *sot_url;
  char *sot_parent;
} spotify_open_track_t;





/**
 * Message for communication with main thread
 */
typedef struct spotify_msg {
  TAILQ_ENTRY(spotify_msg) sm_link;
  spotify_msg_type_t sm_op;
  union {
    void *sm_ptr;
    int sm_int;
  };
} spotify_msg_t;


/**
 * Image load request
 */
typedef struct spotify_image {
  uint8_t *si_id;

  int si_errcode;
  
  pixmap_t *si_pixmap;

} spotify_image_t;

static hts_cond_t spotify_cond_image;


/**
 * Get parent request (typically track -> its albums)
 */
typedef struct spotify_parent {
  const char *sp_uri;
  char *sp_errbuf;
  size_t sp_errlen;

  char *sp_parent;

  int sp_errcode;

  sp_track *sp_track;
  
  LIST_ENTRY(spotify_parent) sp_link;

} spotify_parent_t;

static LIST_HEAD(, spotify_parent) pending_get_parents;

static hts_cond_t spotify_cond_parent;

static void spotify_try_get_parents(void);

static void parse_search_reply(sp_search *result, prop_t *nodes, prop_t *view);

static void handle_artist_url(char *url, sp_albumtype *type, int *albums);

/**
 *
 */
static spotify_msg_t *
spotify_msg_build(spotify_msg_type_t op, void *ptr)
{
  spotify_msg_t *sm = malloc(sizeof(spotify_msg_t));
  sm->sm_op = op;
  sm->sm_ptr = ptr;
  return sm;
}


/**
 *
 */
static spotify_msg_t *
spotify_msg_build_int(spotify_msg_type_t op, int v)
{
  spotify_msg_t *sm = malloc(sizeof(spotify_msg_t));
  sm->sm_op = op;
  sm->sm_int = v;
  return sm;
}


/**
 *
 */
static void
spotify_msg_enq_locked(spotify_msg_t *sm)
{
  TAILQ_INSERT_TAIL(&spotify_msgs, sm, sm_link);
  hts_cond_signal(&spotify_cond_main);
}


/**
 *
 */
static void
spotify_msg_enq(spotify_msg_t *sm)
{
  hts_mutex_lock(&spotify_mutex);
  spotify_msg_enq_locked(sm);
  hts_mutex_unlock(&spotify_mutex);
}


/**
 *
 */
static int
spotify_try_login(sp_session *s, int retry, const char *reason) 
{
  char *username;
  char *password;
  int r;

  prop_set_string(prop_status, "Attempting login");

  r = keyring_lookup("Login to Spotify", &username, &password, NULL, retry,
		     "spotify:", reason);

  if(r == -1) {
    // Login canceled by user
    hts_mutex_lock(&spotify_mutex);
    hts_cond_broadcast(&spotify_cond_login);
    hts_mutex_unlock(&spotify_mutex);
    return -1;
  }

  if(r == 1) {
    /* Nothing found, but we must have a username / password */
    return spotify_try_login(s, 1, NULL);
  }

  f_sp_session_login(s, username, password);

  free(username);
  free(password);
  return 0;
}


/**
 *
 */
static void
spotify_logged_in(sp_session *sess, sp_error error)
{
  sp_user *user;

  if(error == 0) {
    hts_mutex_lock(&spotify_mutex);
    is_logged_in = 1;
    hts_cond_broadcast(&spotify_cond_login);
    hts_mutex_unlock(&spotify_mutex);

    user = f_sp_session_user(sess);

    prop_set_stringf(prop_status, "Logged in as user: %s",
		     f_sp_user_display_name(user));

    load_initial_playlists(f_sp_session_playlistcontainer(sess));

  } else {
    notify_add(NOTIFY_ERROR, NULL, 5, "Spotify: Login failed -- %s",
	       f_sp_error_message(error));
    spotify_try_login(sess, 1, f_sp_error_message(error));

    prop_set_stringf(prop_status, "Login failed: %s ",
		     f_sp_error_message(error));
  }
}


/**
 *
 */
static void
spotify_logged_out(sp_session *sess)
{
  notify_add(NOTIFY_INFO, NULL, 5, "Spotify: Logged out");

  hts_mutex_lock(&spotify_mutex);
  is_logged_in = 0;
  hts_cond_broadcast(&spotify_cond_login);
  hts_mutex_unlock(&spotify_mutex);
}


/**
 *
 */
static void
spotify_connection_error(sp_session *sess, sp_error error)
{
  notify_add(NOTIFY_ERROR, NULL, 5, "Spotify: Connection error\n%s",
	     f_sp_error_message(error));
}


/**
 *
 */
static void
spotify_events_pending(sp_session *sess)
{
  spotify_msg_enq(spotify_msg_build(SPOTIFY_PENDING_EVENT, NULL));
}


/**
 *
 */
static void
spotify_play_token_lost(sp_session *sess)
{
  notify_add(NOTIFY_ERROR, NULL, 5, 
	     "Spotify: Playback paused, another client is using this account");
  if(spotify_mp != NULL)
    mp_enqueue_event(spotify_mp, event_create_type(EVENT_INTERNAL_PAUSE));

}



/**
 * 
 */
static void
spotify_make_link(sp_link *link, char *buf, size_t len)
{
  f_sp_link_as_string(link, buf, len);
  f_sp_link_release(link);
}


/**
 * Processing done, signal back to calling thread
 */
static void
spotify_uri_return(spotify_uri_t *su, int errcode)
{
  if(su->su_playlist_link != NULL)
    f_sp_link_release(su->su_playlist_link);

  if(su->su_track != NULL)
    f_sp_track_release(su->su_track);

  hts_mutex_lock(&spotify_mutex);
  su->su_errcode = errcode;
  hts_cond_broadcast(&spotify_cond_uri);
  hts_mutex_unlock(&spotify_mutex);
}


/**
 *
 */
static void
spotify_end_of_track(sp_session *sess)
{
  media_pipe_t *mp = spotify_mp;

  if(mp != NULL)
    mp_enqueue_event(mp, event_create_type(EVENT_EOF));
}


/**
 *
 */
static int
spotify_music_delivery(sp_session *sess, const sp_audioformat *format, 
		       const void *frames, int num_frames)
{
  media_pipe_t *mp = spotify_mp;
  spotify_uri_t *su = su_playing;
  media_queue_t *mq;
  media_buf_t *mb;

  if(su == NULL || mp == NULL)
    return num_frames;

  mq = &mp->mp_audio;

  if(num_frames == 0) {
    play_position = (int64_t)seek_pos * format->sample_rate / 1000;
    return 0;
  }

  if(mq->mq_len > 100)
    return 0;

  mb = media_buf_alloc();
  mb->mb_data_type = MB_AUDIO;
  
  mb->mb_size = num_frames * 2 * sizeof(int16_t);
  mb->mb_data = malloc(mb->mb_size);
  mb->mb_channels = format->channels;
  mb->mb_rate = format->sample_rate;

  mb->mb_time = play_position * 1000000LL / format->sample_rate;
  play_position += num_frames;

  memcpy(mb->mb_data, frames, mb->mb_size);

  mb_enqueue_always(mp, mq, mb);
  return num_frames;
}

/**
 *
 */
static void
spotify_play_track_try(void)
{
  spotify_uri_t *su = su_pending;
  sp_error err;

  if(su == NULL)
    return;

  err = f_sp_track_error(su->su_track);

  if(err == SP_ERROR_IS_LOADING) {
    TRACE(TRACE_DEBUG, "spotify", 
	  "Track requested for playback is not loaded, retrying");
    return;
  }

  su_pending = NULL;

  if(err == SP_ERROR_OK)
    err = f_sp_session_player_load(spotify_session, su->su_track);

  if(err != SP_ERROR_OK) {
    snprintf(su->su_errbuf, su->su_errlen, "Unable to load track:\n%s",
	     f_sp_error_message(err));
    spotify_uri_return(su, 1);
    return;
  }

  TRACE(TRACE_DEBUG, "spotify", "Starting playback of track: %s (%s)", 
	su->su_uri, f_sp_track_name(su->su_track));

  mp_become_primary(spotify_mp);
  spotify_mp->mp_audio.mq_stream = 0; // Must be set to somthing != -1
  play_position = 0;

  if((err = f_sp_session_player_play(spotify_session, 1))) {
    snprintf(su->su_errbuf, su->su_errlen, "Unable to play track:\n%s",
	     f_sp_error_message(err));
    spotify_uri_return(su, 1);
    return;
  }

  su_playing = su;
  spotify_uri_return(su, 0);
}


/**
 *
 */
static void
spotify_play_track(spotify_uri_t *su)
{
  sp_link *l;

  if((l = f_sp_link_create_from_string(su->su_uri)) == NULL) {
    snprintf(su->su_errbuf, su->su_errlen, "Invalid spotify URI");
    spotify_uri_return(su, 1);
    return;
  }

  if(f_sp_link_type(l) != SP_LINKTYPE_TRACK) {
    snprintf(su->su_errbuf, su->su_errlen, 
	     "Invalid URI for playback (not a track)");
    spotify_uri_return(su, 1);
    f_sp_link_release(l);
    return;
  }

  su->su_track = f_sp_link_as_track(l);
  f_sp_track_add_ref(su->su_track);
  f_sp_link_release(l);

  assert(su_pending == NULL);
  su_pending = su;

  spotify_play_track_try();
}


/**
 *
 */
static void
set_image_uri(prop_t *p, const uint8_t *id)
{
  if(id == NULL)
    return;

  prop_set_stringf(p, "spotify:image:"
		   "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
		   "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
		   id[ 0],id[ 1],id[ 2],id[ 3],id[ 4], 
		   id[ 5],id[ 6],id[ 7],id[ 8],id[ 9], 
		   id[10],id[11],id[12],id[13],id[14], 
		   id[15],id[16],id[17],id[18],id[19]);
}


/**
 *
 */
static void
spotify_metadata_update_track(metadata_t *m)
{
  prop_t *meta = m->m_prop;
  prop_t *p;
  sp_track *track = m->m_source;
  sp_album *album;
  sp_artist *artist;
  char txt[1024];
  char url[URL_MAX];
  int nartists, i;

  if(!f_sp_track_is_loaded(track))
    return;

  if(!f_sp_track_is_available(track) && m->m_plt != NULL) {

    playlist_track_t *plt = m->m_plt;

    if(plt->plt_prop_root != NULL) {
      prop_destroy(plt->plt_prop_root);
      plt->plt_prop_root = NULL;
    }
    return;
  }


  album = f_sp_track_album(track);

  txt[0] = 0;
  nartists = f_sp_track_num_artists(track);
  for(i = 0; i < nartists; i++)
    snprintf(txt + strlen(txt), sizeof(txt) - strlen(txt),
	     "%s%s", strlen(txt) ? ", " : "", 
	     f_sp_artist_name(f_sp_track_artist(track, i)));

  prop_set_string(prop_create(meta, "mediaformat"), "spotify");
  prop_set_string(prop_create(meta, "title"), f_sp_track_name(track));
  prop_set_int(prop_create(meta, "trackindex"), f_sp_track_index(track));
  prop_set_float(prop_create(meta, "duration"), 
		 (float)f_sp_track_duration(track) / 1000.0);

  prop_set_float(prop_create(meta, "popularity"), 
		 (float)f_sp_track_popularity(track) / 100.0);

  if(album != NULL) {
    spotify_make_link(f_sp_link_create_from_album(album), url, sizeof(url));
    prop_set_link(prop_create(meta, "album"), f_sp_album_name(album), url);
    set_image_uri(prop_create(meta, "album_art"), f_sp_album_cover(album));
  }
  
  if(nartists > 0) {
    artist = f_sp_track_artist(track, 0);
    spotify_make_link(f_sp_link_create_from_artist(artist), url, sizeof(url));
    prop_set_link(prop_create(meta, "artist"), txt, url);

  } else {
    prop_set_string(prop_create(meta, "artist"), txt);
  }

  if(!(m->m_flags & METADATA_ARTIST_IMAGES_SCRAPPED) &&
     f_sp_track_num_artists(track) > 0 && 
     (artist = f_sp_track_artist(track, 0)) != NULL) {
    m->m_flags |= METADATA_ARTIST_IMAGES_SCRAPPED;

    p = prop_create(meta, "artist_images");

    scrapper_artist_init(p, f_sp_artist_name(artist));
  }
}


 
/**
 *
 */
static void
spotify_metadata_update_albumname(prop_t *p, sp_album *album)
{
  prop_set_string(p, f_sp_album_name(album));
}

 
/**
 *
 */
static void
spotify_metadata_update_albumyear(prop_t *p, sp_album *album)
{
  int y = f_sp_album_year(album);
  if(y)
    prop_set_int(p, y);
}


/**
 *
 */
static void
spotify_metadata_update_albumartistname(prop_t *p, sp_album *album)
{
  sp_artist *artist = f_sp_album_artist(album);
  if(artist != NULL)
    prop_set_string(p, f_sp_artist_name(artist));
}


/**
 *
 */
static void
spotify_metadata_update_artistname(prop_t *p, sp_artist *artist)
{
  prop_set_string(p, f_sp_artist_name(artist));
}


/**
 *
 */
static void
spotify_metadata_update_albumimage(prop_t *p, sp_album *album)
{
  set_image_uri(p, f_sp_album_cover(album));
}


/**
 *
 */
static void
metadata_update(metadata_t *m)
{
  switch(m->m_type) {
  case METADATA_TRACK:
    spotify_metadata_update_track(m);
    break;
    
  case METADATA_ALBUM_NAME:
    spotify_metadata_update_albumname(m->m_prop, m->m_source);
    break;

  case METADATA_ALBUM_YEAR:
    spotify_metadata_update_albumyear(m->m_prop, m->m_source);
    break;

  case METADATA_ALBUM_ARTIST_NAME:
    spotify_metadata_update_albumartistname(m->m_prop, m->m_source);
    break;

  case METADATA_ALBUM_IMAGE:
    spotify_metadata_update_albumimage(m->m_prop, m->m_source);
    break;
    
  case METADATA_ARTIST_NAME:
    spotify_metadata_update_artistname(m->m_prop, m->m_source);
    break;
  }
}

/**
 *
 */
static void
spotify_metadata_updated(sp_session *sess)
{
  metadata_t *m;

  hts_mutex_lock(&meta_mutex);

  LIST_FOREACH(m, &metadatas, m_link)
    metadata_update(m);

  hts_mutex_unlock(&meta_mutex);

  spotify_play_track_try();
  spotify_try_get_parents();
}

/**
 *
 */
static void
metadata_prop_cb(void *opaque, prop_event_t event, ...)
{
  metadata_t *m = opaque;
  spotify_msg_type_t r;
  prop_t *p;
  prop_sub_t *s;
  va_list ap;
  va_start(ap, event);

  if(event != PROP_DESTROYED) 
    return;

  p = va_arg(ap, prop_t *);
  s = va_arg(ap, prop_sub_t *);

  prop_unsubscribe(s);

  LIST_REMOVE(m, m_link);
  prop_ref_dec(m->m_prop);

  /* Since we are not in the spotify thread, but all accessed to
     the API must go there, we post a request to release
     the reference */
  
  switch(m->m_type) {
  case METADATA_TRACK:
    r = SPOTIFY_RELEASE_TRACK;
    break;
    
  case METADATA_ALBUM_NAME:
  case METADATA_ALBUM_YEAR:
  case METADATA_ALBUM_ARTIST_NAME:
  case METADATA_ALBUM_IMAGE:
    r = SPOTIFY_RELEASE_ALBUM;
    break;

   case METADATA_ARTIST_NAME:
    r = SPOTIFY_RELEASE_ARTIST;
    break;
  default:
    abort();
  }

  spotify_msg_enq(spotify_msg_build(r, m->m_source));
  free(m);
}


static void
metadata_create0(prop_t *p, metadata_type_t type, void *source,
		 playlist_track_t *plt)
{
  metadata_t *m = malloc(sizeof(metadata_t));

  prop_ref_inc(p);
  m->m_plt = plt;
  m->m_prop = p;
  m->m_type = type;
  m->m_source = source;
  m->m_flags = 0;

  switch(m->m_type) {
  case METADATA_TRACK:
    f_sp_track_add_ref(source);
    break;
    
  case METADATA_ALBUM_NAME:
  case METADATA_ALBUM_YEAR:
  case METADATA_ALBUM_IMAGE:
  case METADATA_ALBUM_ARTIST_NAME:
    f_sp_album_add_ref(source);
    break;

  case METADATA_ARTIST_NAME:
    f_sp_artist_add_ref(source);
    break;
  }

  hts_mutex_lock(&meta_mutex);
  LIST_INSERT_HEAD(&metadatas, m, m_link);

  prop_subscribe(PROP_SUB_TRACK_DESTROY,
		 PROP_TAG_CALLBACK, metadata_prop_cb, m,
		 PROP_TAG_MUTEX, &meta_mutex,
		 PROP_TAG_ROOT, p,
		 NULL);
  
  metadata_update(m);
  hts_mutex_unlock(&meta_mutex);
}


/**
 *
 */
static void
metadata_create(prop_t *p, metadata_type_t type, void *source)
{
  metadata_create0(p, type, source, NULL);
}


/**
 *
 */
typedef struct browse_helper {
  prop_t *root;
  prop_t *loadingprop;
  int flat;
  sp_albumtype type;
} browse_helper_t;


/**
 *
 */
static browse_helper_t *
bh_create(prop_t *root, int flat, sp_albumtype type, prop_t *loadingprop)
{
  browse_helper_t *bh = malloc(sizeof(browse_helper_t));

  prop_ref_inc(root);
  bh->root = root;

  if(loadingprop)
    prop_ref_inc(loadingprop);
  bh->loadingprop = loadingprop;

  bh->flat = flat;
  bh->type = type;
  return bh;
}



/**
 * Load an album into the property tree passed via userdata
 */
static void
spotify_browse_album_callback(sp_albumbrowse *result, void *userdata)
{
  browse_helper_t *bh = userdata;
  prop_t *p;
  sp_track *track;
  int i, ntracks;
  char url[URL_MAX];

  ntracks = f_sp_albumbrowse_num_tracks(result);

  for(i = 0; i < ntracks; i++) {
    track = f_sp_albumbrowse_track(result, i);

    p = prop_create(NULL, "node");

    spotify_make_link(f_sp_link_create_from_track(track, 0), 
		      url, sizeof(url));

    prop_set_string(prop_create(p, "url"), url);
    prop_set_string(prop_create(p, "type"), "track");
    metadata_create(prop_create(p, "metadata"), METADATA_TRACK, track);

    if(prop_set_parent(p, bh->root))
      prop_destroy(p);

  }
  f_sp_albumbrowse_release(result);
  spotify_metadata_updated(spotify_session);

  if(bh->loadingprop != NULL) {
    prop_set_int(bh->loadingprop, 0);
    prop_ref_dec(bh->loadingprop);
  }

  prop_ref_dec(bh->root);
  free(bh);
}


/**
 * Helper struct for artist browse
 */
typedef struct album {
  sp_album *album;
  int duration;
  int tracks;
  int firsttrack;
} album_t;


/**
 *
 */
static int
album_cmp(const void *A, const void *B)
{
  const album_t *a = A;
  const album_t *b = B;
  
  if(f_sp_album_year(a->album) > f_sp_album_year(b->album))
    return -1;
  if(f_sp_album_year(a->album) < f_sp_album_year(b->album))
    return 1;

  return strcasecmp(f_sp_album_name(a->album), f_sp_album_name(b->album));
}


/**
 *
 */
static void
artist_add_album_tracks(sp_artistbrowse *result, int first, int num,
			prop_t *root)
{
  int i;
  sp_track *t;
  prop_t *n;
  char url[URL_MAX];

  for(i = first; i < first + num; i++) {

    t = f_sp_artistbrowse_track(result, i);

    n = prop_create(NULL, "node");
    spotify_make_link(f_sp_link_create_from_track(t, 0), url, sizeof(url));
    prop_set_string(prop_create(n, "url"), url);
    
    metadata_create(prop_create(n, "metadata"), METADATA_TRACK, t);
    
    if(prop_set_parent(n, root))
      prop_destroy(n);
  }
}


/**
 *
 */
static sp_albumtype
my_album_type(sp_album *alb, sp_artist *a0)
{
  return f_sp_album_artist(alb) != a0 ? 
    SP_ALBUMTYPE_COMPILATION : f_sp_album_type(alb);
}


/**
 *
 */
static void
spotify_browse_artist_callback(sp_artistbrowse *result, void *userdata)
{
  browse_helper_t *bh = userdata;
  int nalbums = 0, ntracks, i, j;
  sp_album *prev = NULL, *album;
  sp_artist *artist;
  sp_track *t;
  album_t *av;
  char url[URL_MAX];

  // libspotify does not return the albums in any particular order.
  // thus, we need to do some sorting and filtering

  ntracks = f_sp_artistbrowse_num_tracks(result);
  artist = f_sp_artistbrowse_artist(result);

  for(i = 0; i < ntracks; i++) {
    album = f_sp_track_album(f_sp_artistbrowse_track(result, i));
    if(album == prev || !f_sp_album_is_available(album) ||
       my_album_type(album, artist) != bh->type)
      continue;
    nalbums++;
    prev = album;
  }

  av = alloca(nalbums * sizeof(album_t));
  j = 0;
  prev = NULL;
  for(i = 0; i < ntracks; i++) {
    t = f_sp_artistbrowse_track(result, i);
    album = f_sp_track_album(t);

    if(!f_sp_album_is_available(album) || 
       my_album_type(album, artist) != bh->type)
      continue;

    if(album != prev) {
      av[j].duration = 0;
      av[j].tracks = 0;
      av[j].firsttrack = i;
      av[j++].album = album;
      prev = album;
    }
    av[j-1].duration += f_sp_track_duration(t);
    av[j-1].tracks++;
  }

  assert(j == nalbums);

  qsort(av, nalbums, sizeof(album_t), album_cmp);

  for(i = 0; i < nalbums; i++) {
    album_t *a = av + i;
    
    if(bh->flat) {
      // No hierarchy (for playqueue track listing)
    
      artist_add_album_tracks(result, a->firsttrack, a->tracks, bh->root);

    } else {

      prop_t *ar = prop_create(NULL, "node");
      prop_t *meta = prop_create(ar, "metadata");

      prop_set_int(prop_create(meta, "tracks"), a->tracks);
      prop_set_float(prop_create(meta, "duration"), a->duration / 1000.0);
	
      spotify_make_link(f_sp_link_create_from_album(a->album), 
			url, sizeof(url));

      prop_set_string(prop_create(ar, "url"), url);
      prop_set_string(prop_create(ar, "type"), "album");
      
      metadata_create(prop_create(meta, "title"),
		      METADATA_ALBUM_NAME, a->album);
      
      metadata_create(prop_create(meta, "album_name"),
		      METADATA_ALBUM_NAME, a->album);

      metadata_create(prop_create(meta, "album_year"),
		      METADATA_ALBUM_YEAR, a->album);

      metadata_create(prop_create(meta, "album_art"),
		      METADATA_ALBUM_IMAGE, a->album);

      artist_add_album_tracks(result, a->firsttrack, a->tracks,
			      prop_create(ar, "nodes"));
      
      if(prop_set_parent(ar, bh->root))
	prop_destroy(ar);
    }
  }

  f_sp_artistbrowse_release(result);
  spotify_metadata_updated(spotify_session);

  if(bh->loadingprop != NULL) {
    prop_set_int(bh->loadingprop, 0);
    prop_ref_dec(bh->loadingprop);
  }
  prop_ref_dec(bh->root);
  free(bh);
}


/**
 *
 */
static void
add_dir(prop_t *parent, const char *title, const char *url)
{
  prop_t *p, *meta;
  p = prop_create(NULL, NULL);
  
  meta = prop_create(p, "metadata");

  prop_set_string(prop_create(p, "url"), url);
  prop_set_string(prop_create(meta, "title"), title);
  prop_set_string(prop_create(p, "type"), "directory");

  if(prop_set_parent(p, parent))
    prop_destroy(p);
}


/**
 *
 */
static void
spotify_open_artist(sp_link *l, prop_t *p, int albums, 
		    sp_albumtype albtype)
{
  sp_artist *artist = f_sp_link_as_artist(l);
  char url[URL_MAX];
  char prefix[128];
  prop_t *n = prop_create(p, "nodes");

  if(!albums) {

    f_sp_link_as_string(l, prefix, sizeof(prefix));

    snprintf(url, sizeof(url), "%s#albums", prefix);
    add_dir(n, "Albums", url);
    snprintf(url, sizeof(url), "%s#singles", prefix);
    add_dir(n, "Singles", url);
    snprintf(url, sizeof(url), "%s#compilations", prefix);
    add_dir(n, "Compilations", url);

    prop_set_string(prop_create(p, "view"), "list");
    prop_set_int(prop_create(p, "loading"), 0);

  } else {

    prop_set_string(prop_create(p, "view"), "albumcollection");

    f_sp_artistbrowse_create(spotify_session, artist,
			     spotify_browse_artist_callback,
			     bh_create(n, 0, albtype,
				       prop_create(p, "loading")));
  }
  metadata_create(prop_create(p, "title"), METADATA_ARTIST_NAME, artist);
}



/**
 *
 */
static playlist_t *
playlist_find(const char *url)
{
  playlist_t *pl;
  int i = 0;

  for(i = 0; i < ptrvec_size(&playlists); i++) {
    pl = ptrvec_get_entry(&playlists, i);
    if(pl->pl_url != NULL && !strcmp(pl->pl_url, url))
      return pl;
  }
  return NULL;
}


/**
 *
 */
static void
spotify_page_destroy(spotify_page_t *sp)
{
  prop_ref_dec(sp->sp_root);
  free(sp->sp_url);
  free(sp);
}

/**
 *
 */
static void
spotify_open_rootlist(prop_t *p)
{
  prop_set_string(prop_create(p, "view"), "list");
  prop_set_string(prop_create(p, "title"), "Spotify playlists");
  prop_link(prop_playlists, prop_create(p, "nodes"));
  prop_link(prop_syncing_playlists, prop_create(p, "loading"));
}


/**
 *
 */
static void
spotify_open_search_done(sp_search *result, void *userdata)
{
  spotify_page_t *sp = userdata;
  prop_t *nodes = prop_create(NULL, "nodes");

  parse_search_reply(result, nodes, prop_create(sp->sp_root, "view"));

  if(prop_set_parent(nodes, sp->sp_root))
    prop_destroy(nodes);

  prop_set_string(prop_create(sp->sp_root, "type"), "directory");
  prop_set_int(prop_create(sp->sp_root, "loading"), 0);
  spotify_page_destroy(sp);
}

/**
 *
 */
static int
spotify_open_search(spotify_page_t *sp, const char *query)
{
  return f_sp_search_create(spotify_session, query,
			    0, 250, 0, 250, 0, 250, 
			    spotify_open_search_done, sp) == NULL;
}

/**
 *
 */
static void
spotify_open_album(sp_album *alb, prop_t *p)
{
  f_sp_albumbrowse_create(spotify_session, alb, 
			  spotify_browse_album_callback,
			  bh_create(prop_create(p, "nodes"), 0, 0,
				    prop_create(p, "loading")));

  metadata_create(prop_create(p, "album_name"),  METADATA_ALBUM_NAME, alb);
  metadata_create(prop_create(p, "title"),       METADATA_ALBUM_NAME, alb);
  metadata_create(prop_create(p, "album_year"),  METADATA_ALBUM_YEAR, alb);
  metadata_create(prop_create(p, "album_art"),   METADATA_ALBUM_IMAGE, alb);
  metadata_create(prop_create(p, "artist_name"), METADATA_ALBUM_ARTIST_NAME,
		  alb);

  prop_set_string(prop_create(p, "view"), "album");
}

/**
 *
 */
static void
spotify_open_playlist(const char *url, prop_t *p)
{
  playlist_t *pl = playlist_find(url);
  if(pl == NULL)
    return;

  prop_set_string(prop_create(p, "view"), "list");
  prop_link(pl->pl_prop_title, prop_create(p, "title"));
  prop_link(pl->pl_prop_tracks, prop_create(p, "nodes"));
  prop_link(prop_syncing_playlists, prop_create(p, "loading"));
}


/**
 * Fill sp->sp_root with info from sp->sp_url
 */
static void
spotify_open_page(spotify_page_t *sp)
{
  int artist_albums = 0;
  sp_albumtype album_type = 0;
  sp_link *l;
  sp_linktype type;

  if(!strcmp(sp->sp_url, "spotify:playlists")) {
    spotify_open_rootlist(sp->sp_root);
  } else if(!strncmp(sp->sp_url, "spotify:search:",
		     strlen("spotify:search:"))) {
    if(!spotify_open_search(sp, sp->sp_url + strlen("spotify:search:")))
      return;
  } else {
    handle_artist_url(sp->sp_url, &album_type, &artist_albums);

    if((l = f_sp_link_create_from_string(sp->sp_url)) == NULL) {
      spotify_page_destroy(sp);
      return;
    }

    type = f_sp_link_type(l);

    switch(type) {
    case SP_LINKTYPE_ALBUM:
      spotify_open_album(f_sp_link_as_album(l), sp->sp_root);
      break;

    case SP_LINKTYPE_ARTIST:
      spotify_open_artist(l, sp->sp_root, artist_albums, album_type);
      break;

    case SP_LINKTYPE_PLAYLIST:
      spotify_open_playlist(sp->sp_url, sp->sp_root);
      break;
    default:
      break;
    }
    f_sp_link_release(l);
  }
  prop_set_string(prop_create(sp->sp_root, "type"), "directory");
  spotify_page_destroy(sp);
}


/**
 *
 */
static void
spotify_open_track(spotify_open_track_t *sot)
{
  sp_link *l;

  if((l = f_sp_link_create_from_string(sot->sot_url)) != NULL) {

    if(f_sp_link_type(l) == SP_LINKTYPE_TRACK) {
      prop_t *m = prop_create(NULL, "metadata");
      metadata_create(m, METADATA_TRACK, f_sp_link_as_track(l));
      playqueue_play(sot->sot_url, sot->sot_parent, m, 0);
    }
    f_sp_link_release(l);
  }
  free(sot->sot_url);
  free(sot->sot_parent);
  free(sot);
}



/**
 *
 */
static void
spotify_list(spotify_uri_t *su)
{
  sp_link *l;
  sp_linktype type;
  playlist_t *pl;
  prop_t *root = NULL, *nodes;
  
  if((l = f_sp_link_create_from_string(su->su_uri)) == NULL) {
    snprintf(su->su_errbuf, su->su_errlen, "Invalid spotify URI");
    spotify_uri_return(su, 1);
    return;
  }

  type = f_sp_link_type(l);

  switch(type) {
  case SP_LINKTYPE_ALBUM:
    root = prop_create(NULL, NULL);
    f_sp_albumbrowse_create(spotify_session, f_sp_link_as_album(l),
			    spotify_browse_album_callback, 
			    bh_create(prop_create(root, "nodes"), 0, 0, NULL));
    break;

  case SP_LINKTYPE_ARTIST:
    root = prop_create(NULL, NULL);
    f_sp_artistbrowse_create(spotify_session, f_sp_link_as_artist(l),
			     spotify_browse_artist_callback,
			     bh_create(prop_create(root, "nodes"),
				       1, su->su_album_type, NULL));
    break;


  case SP_LINKTYPE_PLAYLIST:
    if((pl = playlist_find(su->su_uri)) == NULL) {
      snprintf(su->su_errbuf, su->su_errlen, "Playlist %s not found",
	       su->su_uri);
      break;
    }

    root = prop_create(NULL, NULL);
    nodes = prop_create(root, "nodes");
    prop_link(pl->pl_prop_tracks, nodes);
    break;

  default:
    snprintf(su->su_errbuf, su->su_errlen, "Can not handle linktype %d",
	     type);
    break;
  }

  f_sp_link_release(l);
  su->su_list = root;
  spotify_uri_return(su, root == NULL ? 1 : 0);
}


/**
 *
 */
static void
parse_search_reply(sp_search *result, prop_t *nodes, prop_t *view)
{
  int i, nalbums, ntracks, nartists;
  sp_album *album;
  sp_artist *artist;
  char link[URL_MAX];
  prop_t *p, *metadata;

  nalbums  = f_sp_search_num_albums(result);
  nartists = f_sp_search_num_artists(result);
  ntracks  = f_sp_search_num_tracks(result);

  /**
   *
   */
  for(i = 0; i < nalbums; i++) {
    album = f_sp_search_album(result, i);
    artist = f_sp_album_artist(album);

    p = prop_create(NULL, "node");

    spotify_make_link(f_sp_link_create_from_album(album), link, sizeof(link));
    prop_set_string(prop_create(p, "url"), link);
    prop_set_string(prop_create(p, "type"), "album");

    metadata = prop_create(p, "metadata");
    prop_set_string(prop_create(metadata, "title"), f_sp_album_name(album));
    prop_set_string(prop_create(metadata, "artist"), f_sp_artist_name(artist));
    set_image_uri(prop_create(metadata, "album_art"), f_sp_album_cover(album));

    if(prop_set_parent(p, nodes))
      prop_destroy(p);
  }

  if(view != NULL) {
    if(nalbums && nartists == 0 && ntracks == 0)
      prop_set_string(view, "albumcollection");
    else
      prop_set_string(view, "list");
  }

  f_sp_search_release(result);
}


/**
 *
 */
static void
spotify_log_message(sp_session *session, const char *msg)
{
  int l = strlen(msg);
  char *s = alloca(l + 1);
  memcpy(s, msg, l + 1);

  if(l > 0 && s[l - 1] == '\n')
    s[l - 1] = 0;
  TRACE(TRACE_DEBUG, "libspotify", "%s", s);
}


/**
 * Session callbacks
 */
static const sp_session_callbacks spotify_session_callbacks = {
  .logged_in           = spotify_logged_in,
  .logged_out          = spotify_logged_out,
  .connection_error    = spotify_connection_error,
  .metadata_updated    = spotify_metadata_updated,
  .notify_main_thread  = spotify_events_pending,
  .music_delivery      = spotify_music_delivery,
  .play_token_lost     = spotify_play_token_lost,
  .end_of_track        = spotify_end_of_track,
  .log_message         = spotify_log_message,
};


/**
 *
 */
static void 
tracks_added(sp_playlist *plist, sp_track * const * tracks,
	     int num_tracks, int position, void *userdata)
{
  playlist_t *pl = userdata;
  sp_track *t;
  playlist_track_t *plt, *before;
  int i, pos, pos2;
  char url[URL_MAX];

  for(i = 0; i < num_tracks; i++) {
    pos2 = pos = position + i;
    plt = calloc(1, sizeof(playlist_track_t));
    t = (sp_track *)tracks[i];
    
    // Find next non-hidden property to insert before
    while((before = ptrvec_get_entry(&pl->pl_tracks, pos2)) != NULL &&
	  before->plt_prop_root == NULL)
      pos2++;
      
    plt->plt_prop_root = prop_create(NULL, NULL);
    plt->plt_track = t;

    prop_set_string(prop_create(plt->plt_prop_root, "type"), "track");

    spotify_make_link(f_sp_link_create_from_track(t, 0), url, sizeof(url));
    prop_set_string(prop_create(plt->plt_prop_root, "url"), url);

    plt->plt_prop_metadata = prop_create(plt->plt_prop_root, "metadata");


    if(prop_set_parent_ex(plt->plt_prop_root, pl->pl_prop_tracks,
			  before ? before->plt_prop_root : NULL, NULL)) {
      abort();
    }

    metadata_create0(plt->plt_prop_metadata, METADATA_TRACK, t, plt);
    ptrvec_insert_entry(&pl->pl_tracks, pos, plt);
  }
  prop_set_int(pl->pl_prop_num_tracks, pl->pl_tracks.size);
}



/**
 *
 */
static int
intcmp_dec(const void *p1, const void *p2)
{
  return *(int *)p2 - *(int *)p1;
}

/**
 *
 */
static void
tracks_removed(sp_playlist *plist, const int *tracks,
	       int num_tracks, void *userdata)
{
  int *positions;
  playlist_t *pl = userdata;
  playlist_track_t *plt;
  int i;

  /* Sort so we always delete from the end. Better safe then sorry */
  positions = alloca(num_tracks * sizeof(int));
  memcpy(positions, tracks, sizeof(int) * num_tracks);
  qsort(positions, num_tracks, sizeof(int), intcmp_dec);

  for(i = 0; i < num_tracks; i++) {
    plt = ptrvec_remove_entry(&pl->pl_tracks, positions[i]);
    if(plt->plt_prop_root != NULL)
      prop_destroy(plt->plt_prop_root);
    free(plt);
  }
  prop_set_int(pl->pl_prop_num_tracks, pl->pl_tracks.size);
}


/**
 *
 */
static void
tracks_moved(sp_playlist *plist, const int *tracks,
	     int num_tracks, int new_position, void *userdata)
{
  playlist_t *pl = userdata;
  int i, pos2;
  int *positions;
  playlist_track_t *plt, *before, **vec;

  /* Sort so we always delete from the end. Better safe then sorry */
  positions = alloca(num_tracks * sizeof(int));
  memcpy(positions, tracks, sizeof(int) * num_tracks);
  qsort(positions, num_tracks, sizeof(int), intcmp_dec);

  before = ptrvec_get_entry(&pl->pl_tracks, new_position);
  vec = alloca(num_tracks * sizeof(playlist_track_t *));

  for(i = 0; i < num_tracks; i++) {
    vec[num_tracks-1-i] = ptrvec_remove_entry(&pl->pl_tracks, positions[i]);
    if(positions[i] < new_position)
      new_position--;
  }
  for(i = num_tracks - 1; i >= 0; i--) {
    plt = vec[i];

    pos2 = new_position;
    while((before = ptrvec_get_entry(&pl->pl_tracks, pos2)) != NULL &&
	  before->plt_prop_root == NULL)
      pos2++;

    before = ptrvec_get_entry(&pl->pl_tracks, pos2);
    ptrvec_insert_entry(&pl->pl_tracks, new_position, plt);

    if(plt->plt_prop_root != NULL)
      prop_move(plt->plt_prop_root, before ? before->plt_prop_root : NULL);
  }
}

/**
 *
 */
static const char *
playlist_name_update(sp_playlist *plist, playlist_t *pl)
{
  const char *name = f_sp_playlist_name(plist);

  prop_set_string(pl->pl_prop_title, name);

  if(name != NULL && !strcmp(name, "-")) {
    prop_set_string(pl->pl_prop_type, "separator");
  } else {
    prop_set_string(pl->pl_prop_type, "directory");
  }
  return name;
}

/**
 *
 */
static void 
playlist_renamed(sp_playlist *plist, void *userdata)
{
  const char *name = playlist_name_update(plist, userdata);
  TRACE(TRACE_DEBUG, "spotify", "Playlist renamed to %s", name);
}


/**
 *
 */
static void
playlist_set_url(sp_playlist *plist, playlist_t *pl)
{
  char url[URL_MAX];

  if(!f_sp_playlist_is_loaded(plist))
    return;

  spotify_make_link(f_sp_link_create_from_playlist(plist), url, sizeof(url));
  pl->pl_url = strdup(url);
  prop_set_string(prop_create(pl->pl_prop_root, "url"), url);
}


/**
 *
 */
static void 
playlist_state_changed(sp_playlist *plist, void *userdata)
{
  playlist_t *pl = userdata;
  playlist_set_url(plist, pl);
}


/**
 * Callbacks for individual playlists
 */
static sp_playlist_callbacks pl_callbacks = {
  .tracks_added     = tracks_added,
  .tracks_removed   = tracks_removed,
  .tracks_moved     = tracks_moved,
  .playlist_renamed = playlist_renamed,
  .playlist_state_changed = playlist_state_changed,
};


/**
 * A new playlist has been added to the users rootlist
 */
static void
playlist_added(sp_playlistcontainer *pc, sp_playlist *plist,
	       int position, void *userdata)
{
  playlist_t *pl = calloc(1, sizeof(playlist_t));
  prop_t *metadata;
  int i, n;

  pl->pl_playlist = plist;
  pl->pl_position = position;
  pl->pl_prop_root = prop_create(prop_playlists, NULL);

  pl->pl_prop_tracks = prop_create(pl->pl_prop_root, "nodes");
  pl->pl_prop_type = prop_create(pl->pl_prop_root, "type");


  metadata = prop_create(pl->pl_prop_root, "metadata");

  pl->pl_prop_title = prop_create(metadata, "title");
  playlist_name_update(plist, pl);

  pl->pl_prop_num_tracks = prop_create(metadata, "tracks");


  ptrvec_insert_entry(&playlists, position, pl);

  n = f_sp_playlist_num_tracks(plist);
  for(i = 0; i < n; i++) {
    sp_track *t = f_sp_playlist_track(plist, i);
    tracks_added(plist, &t, 1, i, pl);
  }

  f_sp_playlist_add_callbacks(plist, &pl_callbacks, pl);

  playlist_set_url(plist, pl);

  TRACE(TRACE_DEBUG, "spotify", "Playlist %d added (%s)", 
	position, f_sp_playlist_name(plist));
}


/**
 * A playlist has been removed
 */
static void
playlist_removed(sp_playlistcontainer *pc, sp_playlist *plist,
		 int position, void *userdata)
{
  playlist_t *pl;
  playlist_track_t *plt;
  int i;

  TRACE(TRACE_DEBUG, "spotify", "Playlist %d removed (%s)", 
	position, f_sp_playlist_name(plist));

  pl = ptrvec_remove_entry(&playlists, position);

  for(i = 0; i < pl->pl_tracks.size; i++) {
    plt = pl->pl_tracks.vec[i];
    if(plt->plt_prop_root != NULL)
      prop_destroy(plt->plt_prop_root);
    free(plt);
  }

  // Destroys all properties, all tracks, etc
  prop_destroy(pl->pl_prop_root);


  free(pl->pl_tracks.vec);
  free(pl->pl_url);
  free(pl);
}


/**
 * A playlist has been moved
 */
static void
playlist_moved(sp_playlistcontainer *pc, sp_playlist *plist,
	       int old_position, int new_position, void *userdata)
{
  playlist_t *pl, *before;


  TRACE(TRACE_DEBUG, "spotify", "Playlist %d (%s) moved to %d", 
	old_position, f_sp_playlist_name(plist), new_position);

  pl = ptrvec_remove_entry(&playlists, old_position);

  if(new_position > old_position)
    new_position--;
  
  ptrvec_insert_entry(&playlists, new_position, pl);

  before = ptrvec_get_entry(&playlists, new_position + 1);

  prop_move(pl->pl_prop_root, before ? before->pl_prop_root : NULL);
}


/**
 *
 */
static void
rootlist_loaded(sp_playlistcontainer *pc, void *userdata)
{
  TRACE(TRACE_DEBUG, "spotify", "Rootlist loaded");
  prop_set_int(prop_syncing_playlists, 0);
}

/**
 * Playlist container callbacks
 */
static sp_playlistcontainer_callbacks pc_callbacks = {
  .playlist_added   = playlist_added,
  .playlist_removed = playlist_removed,
  .playlist_moved   = playlist_moved,
  .container_loaded = rootlist_loaded,
};


/**
 *
 */
static void
load_initial_playlists(sp_playlistcontainer *pc)
{
  int i, n;

  n = f_sp_playlistcontainer_num_playlists(pc);

  for(i = 0; i < n; i++) {
    playlist_added(pc, f_sp_playlistcontainer_playlist(pc, i), i, NULL);
  }

  f_sp_playlistcontainer_add_callbacks(pc, &pc_callbacks, NULL);
}


/**
 *
 */
static void
spotify_got_image(sp_image *image, void *userdata)
{
  spotify_image_t *si = userdata;
  size_t size;
  const void *pixels = f_sp_image_data(image, &size);

  si->si_pixmap = pixmap_alloc_coded(pixels, size, CODEC_ID_MJPEG);

  hts_mutex_lock(&spotify_mutex);
  si->si_errcode = 0;
  hts_cond_broadcast(&spotify_cond_image);
  hts_mutex_unlock(&spotify_mutex);
}


/**
 *
 */
static void
spotify_get_image(spotify_image_t *si)
{
  f_sp_image_add_load_callback(f_sp_image_create(spotify_session, si->si_id),
			       spotify_got_image, si);
}

/**
 *
 */
static void
spotify_get_parent_return(spotify_parent_t *sp, int errcode)
{
  if(sp->sp_track != NULL)
    f_sp_track_release(sp->sp_track);

  hts_mutex_lock(&spotify_mutex);
  sp->sp_errcode = errcode;
  hts_cond_broadcast(&spotify_cond_parent);
  hts_mutex_unlock(&spotify_mutex);
}


/**
 *
 */
static void
spotify_try_get_parent(spotify_parent_t *sp)
{
  sp_error err = f_sp_track_error(sp->sp_track);
  char url[URL_MAX];

  if(err == SP_ERROR_IS_LOADING) {
    TRACE(TRACE_DEBUG, "spotify", 
	  "Track requested for album resolve is not loaded, retrying");
    return;
  }

  if(err != SP_ERROR_OK) {
    snprintf(sp->sp_errbuf, sp->sp_errlen, "Unable to resolve track:%s",
	     f_sp_error_message(err));
    spotify_get_parent_return(sp, 1);
    return;
  }

  sp_album *album = f_sp_track_album(sp->sp_track);

  spotify_make_link(f_sp_link_create_from_album(album), url, sizeof(url));
  sp->sp_parent = strdup(url);
  LIST_REMOVE(sp, sp_link);
  spotify_get_parent_return(sp, 0);
}


/**
 *
 */
static void
spotify_get_parent(spotify_parent_t *sp)
{
  sp_link *l;

  if((l = f_sp_link_create_from_string(sp->sp_uri)) == NULL) {
    snprintf(sp->sp_errbuf, sp->sp_errlen, "Invalid spotify URI");
    spotify_get_parent_return(sp, 1);
    return;
  }

  sp_linktype type = f_sp_link_type(l);

  if(type != SP_LINKTYPE_TRACK) {
    snprintf(sp->sp_errbuf, sp->sp_errlen, "Not a spotify track");
    f_sp_link_release(l);
    spotify_get_parent_return(sp, 1);
    return;
  }

  sp->sp_track = f_sp_link_as_track(l);
  f_sp_track_add_ref(sp->sp_track);
  f_sp_link_release(l);

  LIST_INSERT_HEAD(&pending_get_parents, sp, sp_link);
  spotify_try_get_parent(sp);
}

/**
 *
 */
static void
spotify_try_get_parents(void)
{
  spotify_parent_t *sp, *next;

  for(sp = LIST_FIRST(&pending_get_parents); sp != NULL; sp = next) {
    next = LIST_NEXT(sp, sp_link);
    spotify_try_get_parent(sp);
  }
}

/**
 *
 */
static int
find_cachedir(char *path, size_t pathlen)
{
#if defined(LOCK_EX) && defined(LOCK_NB)
  int i, fd;
  char buf[PATH_MAX];

  mkdir("/tmp/hts", 0770);
  mkdir("/tmp/hts/showtime/", 0770);
  if(mkdir("/tmp/hts/showtime/libspotify", 0770)) {
    if(errno != EEXIST)
      return -1;
  }

  i = 0;
  for(i = 0; i < 64; i++) {
    snprintf(buf, sizeof(buf), "/tmp/hts/showtime/libspotify/%d.lock", i);
    
    fd = open(buf, O_CREAT | O_RDWR, 0770);
    if(fd == -1)
      return -1;

    if(flock(fd, LOCK_EX | LOCK_NB)) {
      close(fd);
      continue;
    }

    snprintf(path, pathlen, "/tmp/hts/showtime/libspotify/%d.cache", i);
    return 0;
  }
#endif
  return 1;
}

/**
 *
 */
static void *
spotify_thread(void *aux)
{
  sp_session_config sesconf;
  sp_error error;
  sp_session *s;
  spotify_msg_t *sm;
  int next_timeout = 0;
  char cache[PATH_MAX];

  sesconf.api_version = SPOTIFY_API_VERSION;

  if(find_cachedir(cache, sizeof(cache)))
    sesconf.cache_location = "/tmp/libspotify";
  else
    sesconf.cache_location = cache;

  sesconf.settings_location = sesconf.cache_location;

  TRACE(TRACE_DEBUG, "spotify", "Cache location: %s", sesconf.cache_location);

  sesconf.application_key = appkey;
  sesconf.application_key_size = sizeof(appkey);
  sesconf.user_agent = "Showtime";
  sesconf.callbacks = &spotify_session_callbacks;
  

  error = f_sp_session_init(&sesconf, &s);
  hts_mutex_lock(&spotify_mutex);
  if(error) {
    exit(4);
    hts_cond_broadcast(&spotify_cond_login);
    hts_mutex_unlock(&spotify_mutex);
    return NULL;
  }

  spotify_session = s;


  spotify_try_login(s, 0, NULL);

  /* Wakeup any sleepers that are waiting for us to start */

  while(1) {
     if(next_timeout == 0) {
      while((sm = TAILQ_FIRST(&spotify_msgs)) == NULL)
	hts_cond_wait(&spotify_cond_main, &spotify_mutex);

    } else {
      while((sm = TAILQ_FIRST(&spotify_msgs)) == NULL)
	if(hts_cond_wait_timeout(&spotify_cond_main,
				 &spotify_mutex, next_timeout))
	  break;
    }

    if(sm != NULL)
      TAILQ_REMOVE(&spotify_msgs, sm, sm_link);
   
    hts_mutex_unlock(&spotify_mutex);

    if(sm != NULL) {
      switch(sm->sm_op) {
      case SPOTIFY_PENDING_EVENT:
	break;
      case SPOTIFY_LOGOUT:
	TRACE(TRACE_DEBUG, "spotify", "Requesting logout");
	f_sp_session_logout(s);
	break;
      case SPOTIFY_OPEN_PAGE:
	spotify_open_page(sm->sm_ptr);
	break;
      case SPOTIFY_OPEN_TRACK:
	spotify_open_track(sm->sm_ptr);
	break;
      case SPOTIFY_LIST:
	spotify_list(sm->sm_ptr);
	break;
      case SPOTIFY_PLAY_TRACK:
	spotify_play_track(sm->sm_ptr);
	break;
      case SPOTIFY_STOP_PLAYBACK:
	f_sp_session_player_unload(s);
	break;
      case SPOTIFY_RELEASE_TRACK:
	f_sp_track_release(sm->sm_ptr);
	break;
      case SPOTIFY_RELEASE_ALBUM:
	f_sp_album_release(sm->sm_ptr);
	break;
      case SPOTIFY_RELEASE_ARTIST:
	f_sp_artist_release(sm->sm_ptr);
	break;
      case SPOTIFY_RELEASE_IMAGE:
	f_sp_image_release(sm->sm_ptr);
	break;
      case SPOTIFY_SEEK:
	if(spotify_mp == NULL)
	  break;

	mp_flush(spotify_mp);
	
	seek_pos = sm->sm_int;
	error = f_sp_session_player_seek(s, sm->sm_int);
	break;

      case SPOTIFY_PAUSE:
	f_sp_session_player_play(s, !sm->sm_int);
	break;

      case SPOTIFY_GET_IMAGE:
	spotify_get_image(sm->sm_ptr);
	break;
      case SPOTIFY_GET_PARENT:
	spotify_get_parent(sm->sm_ptr);
	break;
      }
      free(sm);
    }

    do {
      f_sp_session_process_events(s, &next_timeout);
    } while(next_timeout == 0);

    hts_mutex_lock(&spotify_mutex);
  }
}


/**
 *
 */
static void
spotify_start(void)
{
  hts_mutex_lock(&spotify_mutex);
  
  if(spotify_started == 0) {
    hts_thread_create_detached("spotify", spotify_thread, NULL);
    spotify_started = 1;
  }
}


/**
 *
 */
static void
handle_artist_url(char *url, sp_albumtype *type, int *albums)
{
  if(strncmp(url, "spotify:artist:", strlen("spotify:artist:")) || 
     strlen(url) <= 37)
    return;

  if(!strcmp(url + 37, "#albums")) {
    *type = SP_ALBUMTYPE_ALBUM;
  } else if(!strcmp(url + 37, "#singles")) {
    *type = SP_ALBUMTYPE_SINGLE;
  } else if(!strcmp(url + 37, "#compilations")) {
    *type = SP_ALBUMTYPE_COMPILATION;
  }
  url[37] = 0;
  *albums = 1; // Get albums
}


/**
 *
 */
static int
be_spotify_open(const char *url0, const char *type, const char *parent,
		nav_page_t **npp, char *errbuf, size_t errlen)
{
  nav_page_t *np = NULL;
  char *url = strdup(url0);

  spotify_start();

  if(!strncmp(url, "spotify:track:", strlen("spotify:track:"))) {
    spotify_open_track_t *sot = malloc(sizeof(spotify_open_track_t));

    sot->sot_url = url;
    sot->sot_parent = parent ? strdup(parent) : NULL;
    spotify_msg_enq_locked(spotify_msg_build(SPOTIFY_OPEN_TRACK, sot));

  } else {

    np = nav_page_create(url, sizeof(nav_page_t), NULL,
			 NAV_PAGE_DONT_CLOSE_ON_BACK);
    
    spotify_page_t *sp = malloc(sizeof(spotify_page_t));
    sp->sp_url = url;
    sp->sp_root = np->np_prop_root;
    prop_ref_inc(sp->sp_root);
    
    prop_set_int(prop_create(np->np_prop_root, "loading"), 1);

    spotify_msg_enq_locked(spotify_msg_build(SPOTIFY_OPEN_PAGE, sp));
  }
  *npp = np;
  hts_mutex_unlock(&spotify_mutex);
  return 0;
}

/**
 * Play given track.
 *
 * We only expect this to be called from the playqueue system.
 */
static event_t *
be_spotify_play(const char *url, media_pipe_t *mp, 
		char *errbuf, size_t errlen)
{
  spotify_uri_t su;
  event_t *e, *eof = NULL;
  event_ts_t *ets;
  int hold = 0, lost_focus = 0;
  media_queue_t *mq = &mp->mp_audio;
  
  memset(&su, 0, sizeof(su));

  spotify_start();

  assert(spotify_mp == NULL);
  spotify_mp = mp;

  su.su_uri = url;
  su.su_errbuf = errbuf;
  su.su_errlen = errlen;
  su.su_errcode = -1;
  
  spotify_msg_enq_locked(spotify_msg_build(SPOTIFY_PLAY_TRACK, &su));

  while(su.su_errcode == -1)
    hts_cond_wait(&spotify_cond_uri, &spotify_mutex);

  if(su.su_errcode) {
    spotify_mp = NULL;
    spotify_msg_enq_locked(spotify_msg_build(SPOTIFY_STOP_PLAYBACK, NULL));
    hts_mutex_unlock(&spotify_mutex);
    return NULL;
  }

  hts_mutex_unlock(&spotify_mutex);

  mp_set_play_caps(mp, MP_PLAY_CAPS_SEEK | MP_PLAY_CAPS_PAUSE);

  mp_set_playstatus_by_hold(mp, hold);

  /* Playback successfully started, wait for events */
  while(1) {

    if(eof != NULL) {
      /* End of file, wait a while for queues to drain more */
      e = mp_wait_for_empty_queues(mp, 0);
      if(e == NULL) {
	e = eof;
	eof = NULL;
	break;
      }

    } else {
      e = mp_dequeue_event(mp);
    }

    if(event_is_type (e, EVENT_EOF)) {
      eof = e;
      continue;
    }


    if(event_is_action(e, ACTION_PREV_TRACK) ||
       event_is_action(e, ACTION_NEXT_TRACK) ||
       event_is_action(e, ACTION_STOP) ||
       event_is_type  (e, EVENT_PLAYQUEUE_JUMP)) {
      
      mp_flush(mp);
      break;
      
    } else if(event_is_type(e, EVENT_SEEK)) {

      ets = (event_ts_t *)e;
      spotify_msg_enq(spotify_msg_build_int(SPOTIFY_SEEK, ets->pts / 1000));

    } else if(event_is_action(e, ACTION_PLAYPAUSE) ||
	      event_is_action(e, ACTION_PLAY) ||
	      event_is_action(e, ACTION_PAUSE)) {

      hold = action_update_hold_by_event(hold, e);
      spotify_msg_enq(spotify_msg_build_int(SPOTIFY_PAUSE, hold));
      mp_send_cmd_head(mp, mq, hold ? MB_CTRL_PAUSE : MB_CTRL_PLAY);
      mp_set_playstatus_by_hold(mp, hold);
      lost_focus = 0;

    } else if(event_is_type(e, EVENT_MP_NO_LONGER_PRIMARY)) {

      hold = 1;
      lost_focus = 1;
      spotify_msg_enq(spotify_msg_build_int(SPOTIFY_PAUSE, 1));
      mp_send_cmd_head(mp, mq, MB_CTRL_PAUSE);
      mp_set_playstatus_by_hold(mp, hold);

    } else if(event_is_type(e, EVENT_MP_IS_PRIMARY)) {

      if(lost_focus) {
	hold = 0;
	lost_focus = 0;
	spotify_msg_enq(spotify_msg_build_int(SPOTIFY_PAUSE, 0));
	mp_send_cmd_head(mp, mq, MB_CTRL_PLAY);
	mp_set_playstatus_by_hold(mp, hold);
      }

    } else if(event_is_type(e, EVENT_INTERNAL_PAUSE)) {

      hold = 1;
      lost_focus = 0;
      mp_send_cmd_head(mp, mq, MB_CTRL_PAUSE);
      mp_set_playstatus_by_hold(mp, hold);

    }
    event_unref(e);
  }

  if(eof != NULL)
    event_unref(eof);

  if(hold) {
    // If we were paused, release playback again.
    mp_send_cmd(mp, mq, MB_CTRL_PLAY);
    mp_set_playstatus_by_hold(mp, 0);
  }

  spotify_mp = NULL;
  spotify_msg_enq(spotify_msg_build(SPOTIFY_STOP_PLAYBACK, NULL));
  return e;
}



/**
 *
 */
static prop_t *
be_spotify_list(const char *url0, char *errbuf, size_t errlen)
{
  spotify_uri_t su;
  char *url = mystrdupa(url0);

  memset(&su, 0, sizeof(su));

  handle_artist_url(url, &su.su_album_type, &su.su_artist_albums);

  spotify_start();

  su.su_uri = url;
  su.su_errbuf = errbuf;
  su.su_errlen = errlen;
  su.su_errcode = -1;

  spotify_msg_enq_locked(spotify_msg_build(SPOTIFY_LIST, &su));

  while(su.su_errcode == -1)
    hts_cond_wait(&spotify_cond_uri, &spotify_mutex);
  
  hts_mutex_unlock(&spotify_mutex);
  return su.su_list;
}


/**
 *
 */
static unsigned int
hex2v(int s)
{
  switch(s) {
  case '0' ... '9':
    return s - '0';
  case 'a' ... 'f':
    return s - 'a' + 10;
  case 'A' ... 'F':
    return s - 'A' + 10;
  default:
    return 0;
  }
}



/**
 *
 */
static int
parse_image_url(uint8_t *out, const char *url)
{
  int i;
  uint8_t v;

  if(strncmp(url, "spotify:image:", strlen("spotify:image:")))
    return -1;

  url += strlen("spotify:image:");

  for(i = 0; i < 20; i++) {

    if(*url == 0)
      return -1;

    v = hex2v(*url++);
    if(*url == 0)
      return -1;

    v = (v << 4) | hex2v(*url++);
    *out++ = v;
  }
  return 0;
}



/**
 *
 */
static pixmap_t *
be_spotify_imageloader(const char *url, int want_thumb, const char *theme,
		       char *errbuf, size_t errlen)
{
  spotify_image_t si;
  uint8_t id[20];

  memset(&si, 0, sizeof(si));

  if(parse_image_url(id, url)) {
    snprintf(errbuf, errlen, "Invalid URL for Spotify imageloader");
    return NULL;
  }

  spotify_start();

  si.si_id = id;
  si.si_errcode = -1;

  spotify_msg_enq_locked(spotify_msg_build(SPOTIFY_GET_IMAGE, &si));

  while(si.si_errcode == -1)
    hts_cond_wait(&spotify_cond_image, &spotify_mutex);

  hts_mutex_unlock(&spotify_mutex);

  if(si.si_errcode == 0)
    return si.si_pixmap;

  snprintf(errbuf, errlen, "Unable to load image");
  return NULL;
}

/**
 *
 */
static int
be_spotify_get_parent(const char *url, 
		      char *parent, size_t parentlen,
		      char *errbuf, size_t errlen)
{
  spotify_parent_t sp = {0};

  spotify_start();

  sp.sp_errcode = -1;
  sp.sp_uri = url;

  spotify_msg_enq_locked(spotify_msg_build(SPOTIFY_GET_PARENT, &sp));

  while(sp.sp_errcode == -1)
    hts_cond_wait(&spotify_cond_parent, &spotify_mutex);

  hts_mutex_unlock(&spotify_mutex);

  if(sp.sp_errcode == 0) {
    snprintf(parent, parentlen, "%s", sp.sp_parent);
    free(sp.sp_parent);
    return 0;
  }
  snprintf(errbuf, errlen, "Unable to resolve parent");
  return -1;
}



#ifdef CONFIG_LIBSPOTIFY_LOAD_RUNTIME
/**
 *
 */
static int
be_spotify_dlopen(void)
{
  void *h;
  const char *sym;
  char libname[64];

  snprintf(libname, sizeof(libname), "libspotify.so.%d", SPOTIFY_API_VERSION);

  h = dlopen(libname, RTLD_NOW);
  if(h == NULL) {
    TRACE(TRACE_INFO, "spotify", "Unable to load %s: %s", libname, dlerror());
    return 1;
  }
  if((sym = resolvesym(h)) != NULL) {
    TRACE(TRACE_ERROR, "spotify", "Unable to resolve symbol \"%s\"", sym);
    dlclose(h);
    return 1;
  }
  return 0;
}
#endif

/**
 *
 */
static int
be_spotify_init(void)
{
  prop_t *p;

#ifdef CONFIG_LIBSPOTIFY_LOAD_RUNTIME
  if(be_spotify_dlopen())
    return 1;
#endif

  prop_playlists = prop_create(prop_get_global(), "spotify_playlists");

  prop_syncing_playlists = prop_create(NULL, NULL);
  prop_set_int(prop_syncing_playlists, 1);

  TAILQ_INIT(&spotify_msgs);

  hts_mutex_init(&spotify_mutex);
  hts_cond_init(&spotify_cond_main);
  hts_cond_init(&spotify_cond_login);
  hts_cond_init(&spotify_cond_uri);
  hts_cond_init(&spotify_cond_image);
  hts_cond_init(&spotify_cond_parent);

  /* Metadata tracking */
  hts_mutex_init(&meta_mutex);

  /* Register as a global source */

  p = sd_add_service("spotify", "Spotify", 
		     "bundle://resources/spotify/spotify-core-logo-96x96.png",
		     &prop_status, NULL, "spotify:playlists");

  prop_set_string(prop_status, "Not logged in");
  return 0;
}


/**
 *
 */
static int
be_spotify_canhandle(const char *url)
{
  return !strncmp(url, "spotify", strlen("spotify"));
}


/**
 *
 */
void spotify_shutdown(void);

void
spotify_shutdown(void)
{
  int done;

  hts_mutex_lock(&spotify_mutex);

  if(is_logged_in) {

    done = 0;

    spotify_msg_enq_locked(spotify_msg_build(SPOTIFY_LOGOUT, &done));

    while(is_logged_in)
      if(hts_cond_wait_timeout(&spotify_cond_login, &spotify_mutex, 5000))
	break;
  }

  hts_mutex_unlock(&spotify_mutex);
}


/**
 *
 */
nav_backend_t be_spotify = {
  .nb_init = be_spotify_init,
  .nb_canhandle = be_spotify_canhandle,
  .nb_open = be_spotify_open,
  .nb_play_audio = be_spotify_play,
  .nb_list = be_spotify_list,
  .nb_imageloader = be_spotify_imageloader,
  .nb_get_parent = be_spotify_get_parent,
};
