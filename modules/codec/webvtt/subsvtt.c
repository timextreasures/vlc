/*****************************************************************************
 * subsvtt.c: Decoder for WEBVTT as ISO1446-30 payload
 *****************************************************************************
 * Copyright (C) 2017 VideoLabs, VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_charset.h>
#include <vlc_subpicture.h>
#include <vlc_codec.h>
#include <vlc_stream.h>
#include <vlc_memstream.h>
#include <assert.h>

#include "../codec/substext.h"
#include "../demux/mp4/minibox.h"
#include "webvtt.h"

#include <ctype.h>

//#define SUBSVTT_DEBUG

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

typedef struct webvtt_region_t webvtt_region_t;
typedef struct webvtt_domnode_t webvtt_domnode_t;
typedef struct webvtt_dom_cue_t webvtt_dom_cue_t;

#define WEBVTT_REGION_LINES_COUNT          18
#define WEBVTT_DEFAULT_LINE_HEIGHT_VH    5.33
#define WEBVTT_LINE_TO_HEIGHT_RATIO      1.06

enum webvtt_align_e
{
    WEBVTT_ALIGN_AUTO,
    WEBVTT_ALIGN_LEFT,
    WEBVTT_ALIGN_CENTER,
    WEBVTT_ALIGN_RIGHT,
    WEBVTT_ALIGN_START,
    WEBVTT_ALIGN_END,
};

typedef struct
{
    char *psz_region;
    enum webvtt_align_e vertical;
    bool b_snap_to_lines;
    float line;
    enum webvtt_align_e linealign;
    float position;
    enum webvtt_align_e positionalign;
    float size;
    enum webvtt_align_e align;
} webvtt_cue_settings_t;

struct webvtt_dom_cue_t
{
    char *psz_id;
    mtime_t i_start;
    mtime_t i_stop;
    webvtt_cue_settings_t settings;
    webvtt_domnode_t *p_nodes;
    unsigned i_lines;
};

struct webvtt_region_t
{
    char *psz_id;
    float f_width;
    unsigned i_lines_max_scroll;
    float anchor_x;
    float anchor_y;
    float viewport_anchor_x;
    float viewport_anchor_y;
    bool b_scroll_up;
    webvtt_dom_cue_t *p_cues[WEBVTT_REGION_LINES_COUNT]; /* worst case 1 line == 1 cue */
    webvtt_region_t *p_next;
};

struct webvtt_domnode_t
{
    char *psz_tag;
    char *psz_attrs;
    char *psz_text;

    webvtt_domnode_t *p_parent;
    webvtt_domnode_t *p_next;
    webvtt_domnode_t *p_child;
};

struct decoder_sys_t
{
    webvtt_region_t regions;
    webvtt_region_t **pp_region_append;
};

#define ATOM_iden VLC_FOURCC('i', 'd', 'e', 'n')
#define ATOM_payl VLC_FOURCC('p', 'a', 'y', 'l')
#define ATOM_sttg VLC_FOURCC('s', 't', 't', 'g')
#define ATOM_vttc VLC_FOURCC('v', 't', 't', 'c')
#define ATOM_vtte VLC_FOURCC('v', 't', 't', 'e')
#define ATOM_vttx VLC_FOURCC('v', 't', 't', 'x')

/*****************************************************************************
 *
 *****************************************************************************/

static bool parse_percent( const char *psz, float *value )
{
    char *psz_end;
    float d = us_strtof( psz, &psz_end );
    if( d >= 0.0 && d <= 100.0 && *psz_end == '%' )
        *value = d / 100.0;
    return psz_end != psz;
}

static bool parse_percent_tuple( const char *psz, float *x, float *y )
{
    char *psz_end;
    float a = us_strtof( psz, &psz_end );
    if( psz_end != psz &&
        a >= 0.0 && a <= 100.0 && psz_end && *psz_end == '%' )
    {
        psz = strchr( psz_end, ',' );
        if( psz )
        {
            float b = us_strtof( ++psz, &psz_end );
            if( psz_end != psz &&
                b >= 0.0 && b <= 100.0 && psz_end && *psz_end == '%' )
            {
                *x = a / 100.0;
                *y = b / 100.0;
                return true;
            }
        }
    }
    return false;
}

static void webvtt_cue_settings_ParseTuple( webvtt_cue_settings_t *p_settings,
                                            const char *psz_key, const char *psz_value )
{
    if( !strcmp( psz_key, "vertical" ) )
    {
        if( !strcmp( psz_value, "rl" ) )
            p_settings->vertical = WEBVTT_ALIGN_RIGHT;
        else if( !strcmp( psz_value, "lr" ) )
            p_settings->vertical = WEBVTT_ALIGN_LEFT;
        else
            p_settings->vertical = WEBVTT_ALIGN_AUTO;
    }
    else if( !strcmp( psz_key, "line" ) )
    {
        if( strchr( psz_value, '%' ) )
            parse_percent( psz_value, &p_settings->line );
        // else /* todo */
        const char *psz_align = strchr( psz_value, ',' );
        if( psz_align++ )
        {
            if( !strcmp( psz_align, "center" ) )
                p_settings->linealign = WEBVTT_ALIGN_CENTER;
            else if( !strcmp( psz_align, "end" ) )
                p_settings->linealign = WEBVTT_ALIGN_END;
            else
                p_settings->linealign = WEBVTT_ALIGN_START;
        }
    }
    else if( !strcmp( psz_key, "position" ) )
    {
        parse_percent( psz_value, &p_settings->position );
        const char *psz_align = strchr( psz_value, ',' );
        if( psz_align++ )
        {
            if( !strcmp( psz_align, "line-left" ) )
                p_settings->linealign = WEBVTT_ALIGN_LEFT;
            else if( !strcmp( psz_align, "line-right" ) )
                p_settings->linealign = WEBVTT_ALIGN_RIGHT;
            else if( !strcmp( psz_align, "center" ) )
                p_settings->linealign = WEBVTT_ALIGN_CENTER;
            else
                p_settings->linealign = WEBVTT_ALIGN_AUTO;
        }
    }
    else if( !strcmp( psz_key, "size" ) )
    {
        parse_percent( psz_value, &p_settings->size );
    }
    else if( !strcmp( psz_key, "region" ) )
    {
        free( p_settings->psz_region );
        p_settings->psz_region = strdup( psz_value );
    }
    else if( !strcmp( psz_key, "align" ) )
    {
        if( !strcmp( psz_value, "start" ) )
            p_settings->align = WEBVTT_ALIGN_START;
        else  if( !strcmp( psz_value, "end" ) )
            p_settings->align = WEBVTT_ALIGN_END;
        else  if( !strcmp( psz_value, "left" ) )
            p_settings->align = WEBVTT_ALIGN_LEFT;
        else  if( !strcmp( psz_value, "right" ) )
            p_settings->align = WEBVTT_ALIGN_RIGHT;
        else
            p_settings->align = WEBVTT_ALIGN_CENTER;
    }
}

static void webvtt_cue_settings_Parse( webvtt_cue_settings_t *p_settings,
                                       char *p_str )
{
    char *p_save;
    char *psz_tuple;
    do
    {
        psz_tuple = strtok_r( p_str, " ", &p_save );
        p_str = NULL;
        if( psz_tuple )
        {
            const char *psz_split = strchr( psz_tuple, ':' );
            if( psz_split && psz_split[1] != 0 && psz_split != psz_tuple )
            {
                char *psz_key = strndup( psz_tuple, psz_split - psz_tuple );
                if( psz_key )
                {
                    webvtt_cue_settings_ParseTuple( p_settings, psz_key, psz_split + 1 );
                    free( psz_key );
                }
            }
        }
    } while( psz_tuple );
}

static void webvtt_cue_settings_Clean( webvtt_cue_settings_t *p_settings )
{
    free( p_settings->psz_region );
}

static void webvtt_cue_settings_Init( webvtt_cue_settings_t *p_settings )
{
    p_settings->psz_region = NULL;
    p_settings->vertical = WEBVTT_ALIGN_AUTO;
    p_settings->b_snap_to_lines = true;
    p_settings->line = -1;
    p_settings->linealign = WEBVTT_ALIGN_START;
    p_settings->position = -1;
    p_settings->positionalign = WEBVTT_ALIGN_AUTO;
    p_settings->size = 1.0; /* 100% */
    p_settings->align = WEBVTT_ALIGN_CENTER;
}

/*****************************************************************************
 *
 *****************************************************************************/
#ifdef SUBSVTT_DEBUG
static void webvtt_domnode_Debug( webvtt_domnode_t *p_node, int i_depth )
{
    for( ; p_node ; p_node = p_node->p_next )
    {
        for( int i=0; i<i_depth; i++) printf(" ");
        if( p_node->psz_text )
            printf("TEXT %s\n", p_node->psz_text );
        else
            printf("TAG%s (%s)\n", p_node->psz_tag, p_node->psz_attrs );
        webvtt_domnode_Debug( p_node->p_child, i_depth + 1 );
    }
}
#endif
static void webvtt_domnode_RecursiveDelete( webvtt_domnode_t *p_node )
{
    while( p_node )
    {
        assert( p_node->psz_text == NULL || p_node->p_child == NULL );
        webvtt_domnode_t *p_next = p_node->p_next;

        webvtt_domnode_RecursiveDelete( p_node->p_child );
        free( p_node->psz_tag );
        free( p_node->psz_attrs );
        free( p_node->psz_text );
        free( p_node );

        p_node = p_next;
    }
}

static webvtt_domnode_t * webvtt_domnode_New( webvtt_domnode_t *p_parent )
{
    webvtt_domnode_t *p_node = calloc( 1, sizeof(*p_node) );
    if( p_node )
        p_node->p_parent = p_parent;
    return p_node;
}

static webvtt_domnode_t * webvtt_domnode_getParentByTag( webvtt_domnode_t *p_parent,
                                                         const char *psz_tag )
{
    for( ; p_parent ; p_parent = p_parent->p_parent )
    {
        if( p_parent->psz_tag && psz_tag && !strcmp( p_parent->psz_tag, psz_tag ) )
            break;
    }
    return p_parent;
}

static inline bool IsEndTag( const char *psz )
{
    return psz[1] == '/';
}

/* returns first opening and last chars of next tag, only when valid */
static const char * FindNextTag( const char *psz, const char **ppsz_taglast )
{
    psz = strchr( psz, '<' );
    if( psz )
    {
        *ppsz_taglast = strchr( psz + 1, '>' );
        if( *ppsz_taglast )
        {
            const size_t tagsize = *ppsz_taglast - psz + 1;
            if( tagsize <= 3 )
            {
                if( tagsize < 2 || IsEndTag(psz) )
                    *ppsz_taglast = psz = NULL;
            }
        } else psz = NULL;
    }
    return psz;
}

/* Points to first char of tag name and sets *ppsz_attrs to first of non-name */
static const char *SplitTag( const char *psz_tag, const char **ppsz_attrs )
{
    psz_tag += IsEndTag( psz_tag ) ? 2 : 1;
    const char *p = psz_tag + 1;
    while( !isblank( *p ) && !ispunct( *p ) && *p != '>' && *p != '/' )
        p++;
    *ppsz_attrs = p;
    return psz_tag;
}

/*****************************************************************************
 *
 *****************************************************************************/
static webvtt_dom_cue_t * webvtt_dom_cue_New( mtime_t i_start, mtime_t i_end )
{
    webvtt_dom_cue_t *p_cue = calloc( 1, sizeof(*p_cue) );
    if( p_cue )
    {
        p_cue->psz_id = NULL;
        p_cue->i_start = i_start;
        p_cue->i_stop = i_end;
        p_cue->p_nodes = NULL;
        p_cue->i_lines = 0;
        webvtt_cue_settings_Init( &p_cue->settings );
    }
    return p_cue;
}

static void webvtt_dom_cue_ClearText( webvtt_dom_cue_t *p_cue )
{
    webvtt_domnode_RecursiveDelete( p_cue->p_nodes );
    p_cue->p_nodes = NULL;
    p_cue->i_lines = 0;
}

static void webvtt_dom_cue_Delete( webvtt_dom_cue_t *p_cue )
{
    webvtt_dom_cue_ClearText( p_cue );
    webvtt_cue_settings_Clean( &p_cue->settings );
    free( p_cue->psz_id );
    free( p_cue );
}

/* Returns reduced by one line cue or deletes it */
static webvtt_dom_cue_t * webvtt_dom_cue_Reduced( webvtt_dom_cue_t *p_cue )
{
    if( p_cue->i_lines <= 1 )
    {
        webvtt_dom_cue_Delete( p_cue );
        return NULL;
    }

    for( webvtt_domnode_t *p_node = p_cue->p_nodes;
                           p_node; p_node = p_node->p_next )
    {
        if( !p_node->psz_text )
            continue;
        const char *nl = strchr( p_node->psz_text, '\n' );
        if( nl )
        {
            size_t i_len = strlen( p_node->psz_text );
            size_t i_remain = i_len - (nl - p_node->psz_text);
            char *psz_new = strndup( nl + 1, i_remain );
            free( p_node->psz_text );
            p_node->psz_text = psz_new;
            p_cue->i_lines--;
            return p_cue;
        }
        else
        {
            free( p_node->psz_text );
            p_node->psz_text = NULL;
            /* FIXME: probably can do a local nodes cleanup */
        }
    }

    /* should not happen */
    webvtt_dom_cue_Delete( p_cue );
    return NULL;
}

/*****************************************************************************
 *
 *****************************************************************************/

static void webvtt_region_ParseTuple( webvtt_region_t *p_region,
                                      const char *psz_key, const char *psz_value )
{
    if( !strcmp( psz_key, "id" ) )
    {
        free( p_region->psz_id );
        p_region->psz_id = strdup( psz_value );
    }
    else if( !strcmp( psz_key, "width" ) )
    {
        parse_percent( psz_value, &p_region->f_width );
    }
    else if( !strcmp( psz_key, "regionanchor" ) )
    {
        parse_percent_tuple( psz_value, &p_region->anchor_x,
                                        &p_region->anchor_y );
    }
    else if( !strcmp( psz_key, "viewportanchor" ) )
    {
        parse_percent_tuple( psz_value, &p_region->viewport_anchor_x,
                                        &p_region->viewport_anchor_y );
    }
    else if( !strcmp( psz_key, "lines" ) )
    {
        int i = atoi( psz_value );
        if( i > 0 )
            p_region->i_lines_max_scroll = __MIN(i, WEBVTT_REGION_LINES_COUNT);
    }
    else if( !strcmp( psz_key, "scroll" ) )
    {
        p_region->b_scroll_up = !strcmp( psz_value, "up" );
    }
}

static void webvtt_region_Parse( webvtt_region_t *p_region, char *psz_line )
{
    char *p_save;
    char *psz_tuple;
    char *p_str = psz_line;
    do
    {
        psz_tuple = strtok_r( p_str, " ", &p_save );
        p_str = NULL;
        if( psz_tuple )
        {
            const char *psz_split = strchr( psz_tuple, ':' );
            if( psz_split && psz_split[1] != 0 && psz_split != psz_tuple )
            {
                char *psz_key = strndup( psz_tuple, psz_split - psz_tuple );
                if( psz_key )
                {
                    webvtt_region_ParseTuple( p_region, psz_key, psz_split + 1 );
                    free( psz_key );
                }
            }
        }
    } while( psz_tuple );
}

static unsigned webvtt_region_CountLines( const webvtt_region_t *p_region )
{
    unsigned i_lines = 0;
    for( size_t i=0; i<WEBVTT_REGION_LINES_COUNT; i++ )
        if( p_region->p_cues[i] )
            i_lines += p_region->p_cues[i]->i_lines;
    return i_lines;
}

static void webvtt_region_ClearCues( webvtt_region_t *p_region )
{
    for( size_t i=0; i<WEBVTT_REGION_LINES_COUNT; i++ )
    {
        if( p_region->p_cues[i] == NULL )
            continue;
        webvtt_dom_cue_Delete( p_region->p_cues[i] );
        p_region->p_cues[i] = NULL;
    }
}

static void webvtt_region_ClearCuesByTime( webvtt_region_t *p_region, mtime_t i_time )
{
    for( size_t i=0; i<WEBVTT_REGION_LINES_COUNT; i++ )
    {
        if( p_region->p_cues[i] == NULL )
            continue;
        if( p_region->p_cues[i]->i_stop <= i_time )
        {
            webvtt_dom_cue_Delete( p_region->p_cues[i] );
            for( size_t j=i; j<WEBVTT_REGION_LINES_COUNT - 1; j++ )
                p_region->p_cues[j]  = p_region->p_cues[j] + 1;
            p_region->p_cues[WEBVTT_REGION_LINES_COUNT - 1] = NULL;
        }
    }
}

static void webvtt_region_Clean( webvtt_region_t *p_region )
{
    webvtt_region_ClearCues( p_region );
    free( p_region->psz_id );
}

/* Remove top most line/cue for bottom insert */
static void webvtt_region_Reduce( webvtt_region_t *p_region )
{
    if( p_region->p_cues[0] )
    {
        webvtt_dom_cue_Delete( p_region->p_cues[0] );
    }
    else
    {
        for( int i=0; i<WEBVTT_REGION_LINES_COUNT; i++ )
        {
            if( p_region->p_cues[i] )
            {
                p_region->p_cues[i] =
                        webvtt_dom_cue_Reduced( p_region->p_cues[i] );
                break;
            }
        }
    }
}

static void webvtt_region_ScrollUp( webvtt_region_t *p_region )
{
    if( p_region->p_cues[0] )
        webvtt_dom_cue_Delete( p_region->p_cues[0] );

    memmove( &p_region->p_cues[0], &p_region->p_cues[1],
            (WEBVTT_REGION_LINES_COUNT - 1) * sizeof(p_region->p_cues[0]) );
    p_region->p_cues[WEBVTT_REGION_LINES_COUNT - 1] = NULL;
}

static void webvtt_region_AddCue( webvtt_region_t *p_region,
                                  webvtt_dom_cue_t *p_cue )
{
    if( p_region->b_scroll_up == false )
    {
        webvtt_region_ClearCues( p_region );
    }
    else
    {
        while( p_cue->i_lines > p_region->i_lines_max_scroll ) /* eh eh */
        {
            p_cue = webvtt_dom_cue_Reduced( p_cue );
            assert( p_cue );
            if( unlikely(!p_cue) )
                return;
        }

        while( webvtt_region_CountLines( p_region ) + p_cue->i_lines
               > p_region->i_lines_max_scroll )
            webvtt_region_Reduce( p_region );

        /* now move everything up */
        webvtt_region_ScrollUp( p_region );
    }
    p_region->p_cues[WEBVTT_REGION_LINES_COUNT - 1] = p_cue;
}

static void webvtt_region_Init( webvtt_region_t *p_region )
{
    p_region->psz_id = NULL;
    p_region->p_next = NULL;
    p_region->f_width = 1.0; /* 100% */
    p_region->anchor_x = 0;
    p_region->anchor_y = 1.0; /* 100% */
    p_region->i_lines_max_scroll = 3;
    p_region->viewport_anchor_x = 0;
    p_region->viewport_anchor_y = 1.0; /* 100% */
    p_region->b_scroll_up = false;
    for( int i=0; i<WEBVTT_REGION_LINES_COUNT; i++ )
        p_region->p_cues[i] = NULL;
}

static void webvtt_region_Delete( webvtt_region_t *p_region )
{
    webvtt_region_Clean( p_region );
    free( p_region );
}

static webvtt_region_t * webvtt_region_New( void )
{
    webvtt_region_t *p_region = malloc(sizeof(*p_region));
    if( p_region )
        webvtt_region_Init( p_region );
    return p_region;
}

static webvtt_region_t * webvtt_region_GetByID( decoder_sys_t *p_sys,
                                                const char *psz_id )
{
    webvtt_region_t *p_region = &p_sys->regions;
    if( psz_id != NULL )
    {
        for( p_region = p_sys->regions.p_next;
             p_region; p_region = p_region->p_next )
        {
            if( !strcmp( psz_id, p_region->psz_id ) )
                return p_region;
        }
    }
    return p_region;
}

/*****************************************************************************
 *
 *****************************************************************************/
static unsigned CountNewLines( const char *psz )
{
    unsigned i = 0;
    while( psz && *psz )
        psz = strchr( psz + 1, '\n' );
    return i;
}

static webvtt_domnode_t * CreateDomNodes( const char *psz_text, unsigned *pi_lines )
{
    webvtt_domnode_t *p_head = NULL;
    webvtt_domnode_t **pp_append = &p_head;
    webvtt_domnode_t *p_parent = p_head;
    *pi_lines = 0;

    while( *psz_text )
    {
        const char *psz_taglast;
        const char *psz_tag = FindNextTag( psz_text, &psz_taglast );
        if( psz_tag )
        {
            if( psz_tag - psz_text > 0 )
            {
                webvtt_domnode_t *p_node = webvtt_domnode_New( p_parent );
                if( p_node )
                {
                    p_node->psz_text = strndup( psz_text, psz_tag - psz_text );
                    *pi_lines += ((*pi_lines == 0) ? 1 : 0) + CountNewLines( p_node->psz_text );
                    *pp_append = p_node;
                    pp_append = &p_node->p_next;
                }
            }

            if( ! IsEndTag( psz_tag ) )
            {
                webvtt_domnode_t *p_node = webvtt_domnode_New( p_parent );
                if( p_node )
                {
                    const char *psz_attrs = NULL;
                    const char *psz_name = SplitTag( psz_tag, &psz_attrs );
                    p_node->psz_tag = strndup( psz_name, psz_attrs - psz_name );
                    if( psz_attrs != psz_taglast )
                        p_node->psz_attrs = strndup( psz_attrs, psz_taglast - psz_attrs );
                    *pp_append = p_node;
                    p_parent = p_node;
                    pp_append = &p_node->p_child;
                }
            }
            else
            {
                if( p_parent )
                {
                    const char *psz_attrs = NULL;
                    const char *psz_name = SplitTag( psz_tag, &psz_attrs );
                    char *psz_tagname = strndup( psz_name, psz_attrs - psz_name );

                    /* Close at matched parent node level due to unclosed tags
                     * like <b><v stuff>foo</b> */
                    p_parent = webvtt_domnode_getParentByTag( p_parent->p_parent, psz_tagname );
                    if( p_parent ) /* continue as parent next */
                        pp_append = &p_parent->p_next;
                    else /* back as top node */
                        pp_append = &p_head->p_next;
                    while( *pp_append )
                        pp_append = &((*pp_append)->p_next);

                    free( psz_tagname );
                }
                else break; /* End tag for non open tag */
            }
            psz_text = psz_taglast + 1;
        }
        else /* Special case: end */
        {
            webvtt_domnode_t *p_node = webvtt_domnode_New( p_parent );
            if( p_node )
            {
                p_node->psz_text = strdup( psz_text );
                *pi_lines += ((*pi_lines == 0) ? 1 : 0) + CountNewLines( p_node->psz_text );
                *pp_append = p_node;
            }
            break;
        }
    }

    return p_head;
}

static void ExpireCues( decoder_t *p_dec, mtime_t i_time )
{
    for( webvtt_region_t *p_vttregion = &p_dec->p_sys->regions;
                          p_vttregion; p_vttregion = p_vttregion->p_next )
    {
        webvtt_region_ClearCuesByTime( p_vttregion, i_time );
    }
}

static void ProcessCue( decoder_t *p_dec, const char *psz, webvtt_dom_cue_t *p_cue )
{
    VLC_UNUSED(p_dec);

    if( p_cue->p_nodes )
        return;
    p_cue->p_nodes = CreateDomNodes( psz, &p_cue->i_lines );
#ifdef SUBSVTT_DEBUG
    webvtt_domnode_Debug( p_cue->p_nodes, 0 );
#endif
}

static text_style_t * InheritStyles( decoder_t *p_dec, const webvtt_domnode_t *p_node )
{
    VLC_UNUSED(p_dec);

    text_style_t *p_style = NULL;
    for( ; p_node; p_node = p_node->p_parent )
    {
        if( p_node->psz_tag )
        {
            if ( !strcmp( p_node->psz_tag, "b" ) )
            {
                if( p_style || (p_style = text_style_Create( STYLE_NO_DEFAULTS )) )
                {
                    p_style->i_style_flags |= STYLE_BOLD;
                    p_style->i_features |= STYLE_HAS_FLAGS;
                }
            }
            else if ( !strcmp( p_node->psz_tag, "i" ) )
            {
                if( p_style || (p_style = text_style_Create( STYLE_NO_DEFAULTS )) )
                {
                    p_style->i_style_flags |= STYLE_ITALIC;
                    p_style->i_features |= STYLE_HAS_FLAGS;
                }
            }
            else if ( !strcmp( p_node->psz_tag, "u" ) )
            {
                if( p_style || (p_style = text_style_Create( STYLE_NO_DEFAULTS )) )
                {
                    p_style->i_style_flags |= STYLE_UNDERLINE;
                    p_style->i_features |= STYLE_HAS_FLAGS;
                }
            }
            else if ( !strcmp( p_node->psz_tag, "v" ) && p_node->psz_attrs )
            {
                if( p_style || (p_style = text_style_Create( STYLE_NO_DEFAULTS )) )
                {
                    unsigned a = 0;
                    for( char *p = p_node->psz_attrs; *p; p++ )
                        a = (a << 3) ^ *p;
                    p_style->i_font_color = (0x7F7F7F | a) & 0xFFFFFF;
                    p_style->i_features |= STYLE_HAS_FONT_COLOR;
                }
            }
        }
    }
    return p_style;
}

struct render_variables_s
{
    const webvtt_region_t *p_region;
    float i_left_offset;
    float i_left;
    float i_top_offset;
    float i_top;
};

static text_segment_t *ConvertNodesToSegments( decoder_t *p_dec,
                                               struct render_variables_s *p_vars,
                                               const webvtt_dom_cue_t *p_cue,
                                               const webvtt_domnode_t *p_node )
{
    text_segment_t *p_head = NULL;
    text_segment_t **pp_append = &p_head;
    for( ; p_node ; p_node = p_node->p_next )
    {
        if( p_node->psz_text )
        {
            *pp_append = text_segment_New( p_node->psz_text );
            if( *pp_append )
            {
                if( (*pp_append)->psz_text )
                    vlc_xml_decode( (*pp_append)->psz_text );
                (*pp_append)->style = InheritStyles( p_dec, p_node );
                pp_append = &((*pp_append)->p_next);
            }
        }

        *pp_append = ConvertNodesToSegments( p_dec, p_vars, p_cue, p_node->p_child );
        while( *pp_append )
            pp_append = &((*pp_append)->p_next);
    }
    return p_head;
}

static text_segment_t *ConvertCueToSegments( decoder_t *p_dec,
                                             struct render_variables_s *p_vars,
                                             const webvtt_dom_cue_t *p_cue )
{
    return ConvertNodesToSegments( p_dec, p_vars, p_cue, p_cue->p_nodes );
}

static void RenderRegions( decoder_t *p_dec, mtime_t i_start, mtime_t i_stop )
{
    subpicture_t *p_spu = NULL;
    subpicture_updater_sys_region_t *p_updtregion = NULL;

    for( webvtt_region_t *p_vttregion = &p_dec->p_sys->regions;
                          p_vttregion; p_vttregion = p_vttregion->p_next )
    {
        text_segment_t *p_segments = NULL;
        text_segment_t **pp_append = &p_segments;

        /* Variables */
        struct render_variables_s v;
        v.p_region = p_vttregion;
        v.i_left_offset = p_vttregion->anchor_x * p_vttregion->f_width;
        v.i_left = p_vttregion->viewport_anchor_x - v.i_left_offset;
        v.i_top_offset = p_vttregion->anchor_y * p_vttregion->i_lines_max_scroll *
                         WEBVTT_DEFAULT_LINE_HEIGHT_VH / 100.0;
        v.i_top = p_vttregion->viewport_anchor_y - v.i_top_offset;
        /* !Variables */

        for( int i=0; i<WEBVTT_REGION_LINES_COUNT; i++ )
        {
            if( !p_vttregion->p_cues[i] ||
                p_vttregion->p_cues[i]->i_start > i_start ||
                p_vttregion->p_cues[i]->i_stop <= i_start )
                continue;

            text_segment_t *p_new = ConvertCueToSegments( p_dec, &v,
                                                          p_vttregion->p_cues[i] );
            if( p_new )
            {
                if( p_segments ) /* auto newlines */
                {
                    *pp_append = text_segment_New( "\n" );
                    if( *pp_append )
                        pp_append = &((*pp_append)->p_next);
                }

                *pp_append = p_new;
                while( *pp_append )
                    pp_append = &((*pp_append)->p_next);
            }
        }
        if( !p_segments )
            continue;

        if( p_spu == NULL )
        {
            p_spu = decoder_NewSubpictureText( p_dec );
            if( p_spu )
            {
                p_updtregion = &p_spu->updater.p_sys->region;
                p_spu->i_start = i_start;
                p_spu->i_stop  = i_stop;
            }
        }
        else
        {
            subpicture_updater_sys_region_t *p_new =
                                    SubpictureUpdaterSysRegionNew( );
            if( p_new )
            {
                SubpictureUpdaterSysRegionAdd( p_updtregion, p_new );
                p_updtregion = p_new;
            }
        }

        if( !p_spu || !p_updtregion )
        {
            text_segment_ChainDelete( p_segments );
            continue;
        }


        if( p_vttregion == &p_dec->p_sys->regions )
        {
            p_updtregion->align = SUBPICTURE_ALIGN_BOTTOM;
        }
        else
        {
            p_updtregion->align = SUBPICTURE_ALIGN_TOP|SUBPICTURE_ALIGN_LEFT;
            p_updtregion->origin.x = v.i_left;
            p_updtregion->origin.y = v.i_top;
            p_updtregion->extent.x = p_vttregion->f_width;
        }
        p_updtregion->flags = UPDT_REGION_ORIGIN_X_IS_RATIO|UPDT_REGION_ORIGIN_Y_IS_RATIO
                            | UPDT_REGION_EXTENT_X_IS_RATIO;
        p_updtregion->p_segments = p_segments;
    }

    if( p_spu )
    {
        p_spu->b_ephemer  = true; /* !important */
        p_spu->b_absolute = false;

        subpicture_updater_sys_t *p_spu_sys = p_spu->updater.p_sys;
        p_spu_sys->p_default_style->f_font_relsize = WEBVTT_DEFAULT_LINE_HEIGHT_VH /
                                                     WEBVTT_LINE_TO_HEIGHT_RATIO;
        decoder_QueueSub( p_dec, p_spu );
    }
}

static int ProcessISOBMFF( decoder_t *p_dec,
                           const uint8_t *p_buffer, size_t i_buffer,
                           mtime_t i_start, mtime_t i_stop )
{
    mp4_box_iterator_t it;
    mp4_box_iterator_Init( &it, p_buffer, i_buffer );
    while( mp4_box_iterator_Next( &it ) )
    {
        if( it.i_type == ATOM_vttc || it.i_type == ATOM_vttx )
        {
            webvtt_dom_cue_t *p_cue = webvtt_dom_cue_New( i_start, i_stop );
            if( !p_cue )
                continue;

            mp4_box_iterator_t vtcc;
            mp4_box_iterator_Init( &vtcc, it.p_payload, it.i_payload );
            while( mp4_box_iterator_Next( &vtcc ) )
            {
                char *psz = NULL;
                switch( vtcc.i_type )
                {
                    case ATOM_iden:
                        free( p_cue->psz_id );
                        p_cue->psz_id = strndup( (char *) vtcc.p_payload, vtcc.i_payload );
                        break;
                    case ATOM_sttg:
                    {
                        psz = strndup( (char *) vtcc.p_payload, vtcc.i_payload );
                        if( psz )
                            webvtt_cue_settings_Parse( &p_cue->settings, psz );
                    } break;
                    case ATOM_payl:
                    {
                        psz = strndup( (char *) vtcc.p_payload, vtcc.i_payload );
                        if( psz )
                            ProcessCue( p_dec, psz, p_cue );
                    } break;
                }
                free( psz );
            }

            webvtt_region_t *p_region = webvtt_region_GetByID( p_dec->p_sys,
                                                               p_cue->settings.psz_region );
            if( p_region == NULL )
                p_region = webvtt_region_GetByID( p_dec->p_sys, NULL /*defaut region*/ );
            assert( p_region );
            webvtt_region_AddCue( p_region, p_cue );
        }
    }
    return 0;
}

struct parser_ctx
{
    webvtt_region_t *p_region;
    decoder_t *p_dec;
};

static void ParserHeaderHandler( void *priv, enum webvtt_header_line_e s,
                                 bool b_new, const char *psz_line )
{
    struct parser_ctx *ctx = (struct parser_ctx *)priv;
    decoder_t *p_dec = ctx->p_dec;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( b_new || !psz_line /* commit */ )
    {
        if( ctx->p_region )
        {
            if( ctx->p_region->psz_id )
            {
                msg_Dbg( p_dec, "added new region %s", ctx->p_region->psz_id );
                *p_sys->pp_region_append = ctx->p_region;
                p_sys->pp_region_append = &ctx->p_region->p_next;
            }
            /* incomplete region decl (no id at least) */
            else webvtt_region_Delete( ctx->p_region );
            ctx->p_region = NULL;
        }

        if( !psz_line )
            return;

        if( b_new )
        {
            if( s == WEBVTT_HEADER_REGION )
                ctx->p_region = webvtt_region_New();
            return;
        }
    }

    if( s == WEBVTT_HEADER_REGION && ctx->p_region )
        webvtt_region_Parse( ctx->p_region, (char*) psz_line );
}

static void LoadExtradata( decoder_t *p_dec )
{
    stream_t *p_stream = vlc_stream_MemoryNew( p_dec,
                                               p_dec->fmt_in.p_extra,
                                               p_dec->fmt_in.i_extra,
                                               true );
    if( !p_stream )
        return;

   struct parser_ctx ctx;
   ctx.p_region = NULL;
   ctx.p_dec = p_dec;
   webvtt_text_parser_t *p_parser =
           webvtt_text_parser_New( &ctx, NULL, NULL, ParserHeaderHandler );
   if( p_parser )
   {
        char *psz_line;
        while( (psz_line = vlc_stream_ReadLine( p_stream )) )
            webvtt_text_parser_Feed( p_parser, psz_line );
        webvtt_text_parser_Delete( p_parser );
        /* commit using null */
        ParserHeaderHandler( &ctx, 0, false, NULL );
   }

    vlc_stream_Delete( p_stream );
}

/****************************************************************************
 * DecodeBlock: decoder data entry point
 ****************************************************************************/
static int DecodeBlock( decoder_t *p_dec, block_t *p_block )
{
    if( p_block == NULL ) /* No Drain */
        return VLCDEC_SUCCESS;

    ExpireCues( p_dec, p_block->i_dts );

    ProcessISOBMFF( p_dec, p_block->p_buffer, p_block->i_buffer,
                    p_block->i_pts, p_block->i_pts + p_block->i_length );

    RenderRegions( p_dec, p_block->i_pts, p_block->i_pts + p_block->i_length );

    block_Release( p_block );
    return VLCDEC_SUCCESS;
}

/*****************************************************************************
 * CloseDecoder: clean up the decoder
 *****************************************************************************/
void CloseDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t *)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    while( p_sys->regions.p_next )
    {
        webvtt_region_t *p_next = p_sys->regions.p_next->p_next;
        webvtt_region_Clean( p_sys->regions.p_next );
        free( p_sys->regions.p_next );
        p_sys->regions.p_next = p_next;
    }
    webvtt_region_Clean( &p_sys->regions );

    free( p_sys );
}

/*****************************************************************************
 * OpenDecoder: probe the decoder and return score
 *****************************************************************************/
int OpenDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;

    if( p_dec->fmt_in.i_codec != VLC_CODEC_WEBVTT )
        return VLC_EGENERIC;

    /* Allocate the memory needed to store the decoder's structure */
    p_dec->p_sys = p_sys = calloc( 1, sizeof( *p_sys ) );
    if( unlikely( p_sys == NULL ) )
        return VLC_ENOMEM;

    webvtt_region_Init( &p_sys->regions );
    p_sys->pp_region_append = &p_sys->regions.p_next;

    p_dec->pf_decode = DecodeBlock;

    if( p_dec->fmt_in.i_extra )
        LoadExtradata( p_dec );

    return VLC_SUCCESS;
}
