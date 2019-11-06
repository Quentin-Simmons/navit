/*
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2008 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */


#define _FILE_OFFSET_BITS 64
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#include <stdlib.h>
#include <glib.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <math.h>
#ifndef _MSC_VER
#include <getopt.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>
#include "file.h"
#include "item.h"
#include "map.h"
#include "zipfile.h"
#include "main.h"
#include "config.h"
#include "linguistics.h"
#include "plugin.h"

#include "maptool.h"

GList *aux_tile_list;
struct tile_head *tile_head_root;
GHashTable *strings_hash,*tile_hash,*tile_hash2;

static char* string_hash_lookup( const char* key ) {
    char* key_ptr = NULL;

    if ( strings_hash == NULL ) {
        strings_hash = g_hash_table_new(g_str_hash, g_str_equal);
    }

    if ( ( key_ptr = g_hash_table_lookup(strings_hash, key )) == NULL ) {
        key_ptr = g_strdup( key );
        g_hash_table_insert(strings_hash, key_ptr,  (gpointer)key_ptr );

    }
    return key_ptr;
}

static char** th_get_subtile( const struct tile_head* th, int idx ) {
    char* subtile_ptr = NULL;
    subtile_ptr = (char*)th + sizeof( struct tile_head ) + idx * sizeof( char *);
    return (char**)subtile_ptr;
}

/**
 * @brief check if the given rectangle fit into any of the four sub tiles of
 * a given bbox.
 *
 * @param r - given rectangle
 * @param bbox - tile bbox
 * @param overlap - overlap sizte in percent
 * @return tile number (a,b,c,d) or 0 if fits in none of them
 */
static char fits_sub_tile (struct rect * r, struct rect * bbox, int overlap) {
    struct coord center;
    int xo;
    int yo;
    /* calculate the new center point */
    center.x = (bbox->l.x + bbox->h.x) / 2;
    center.y = (bbox->l.y + bbox->h.y) / 2;
    /* calculate the x overlap */
    xo = (bbox->h.x - bbox->l.x) * overlap / 100;
    /* calculate the y overlap */
    yo = (bbox->h.y - bbox->l.y) * overlap / 100;

    if (contains_bbox(bbox->l.x,bbox->l.y,center.x+xo,center.y+yo, r)) {
        bbox->h.x = center.x;
        bbox->h.y = center.y;
        return 'd';
    } else if (contains_bbox(center.x-xo,bbox->l.y,bbox->h.x,center.y+yo, r)) {
        bbox->l.x=center.x;
        bbox->h.y=center.y;
        return 'c';
    } else if (contains_bbox(bbox->l.x,center.y-yo,center.x+xo,bbox->h.y, r)) {
        bbox->h.x = center.x;
        bbox->l.y = center.y;
        return 'b';
    } else if (contains_bbox(center.x-xo,center.y-yo,bbox->h.x,bbox->h.y, r)) {
        bbox->l.x=center.x;
        bbox->l.y=center.y;
        return 'a';
    } else {
        /* bbox unchanged, no match */
        return 0;
    }
}

/**
 * @brief Calculate the tile address of a guiven map rectangle
 *
 * @param r - rectangle
 * @param suffix - additional text to add to the tile address
 * @param ret - buffer to write tile address into. Must be at least 15 + strlen(suffix) long
 * @param max - smallest tile number to check rectangle against.
 * @param overlap - allow the items within the tiles to overlap by %
 * @param tr - returns the choosen tile size without overlaps.
 *
 * @returns number of tile depth
 */
int tile(struct rect *r, char *suffix, char *ret, int max, int overlap, struct rect *tr) {
    struct rect bbox;
    struct rect rr;
    int tile;

    /* start with the world bbox */
    bbox=world_bbox;

    /* remember the rect we try to tile */
    rr = *r;

    /* there is no need to forcefully limit the rectangle. It will
     * end up in tile 0 anyways in case it is too big */

    /* init suffix string */
    ret[0] = 0;

    /* iterate through the tile levels as long as it fits in */
    for(tile=0; tile < max; tile ++) {
        char next_tile;

        /* always check without overlap first */
        next_tile = fits_sub_tile (&rr, &bbox, 0);
        /* if we have overlap, re try with overlap */
        if(overlap && next_tile == 0) {
            next_tile = fits_sub_tile (&rr, &bbox, overlap);
        }
        /* did it fit? */
        if(next_tile == 0) {
            /* cannot move this deeper, doesn't fit in no subtile */
            break;
        }
        /* add tile to address */
        ret[tile] = next_tile;
        ret[tile +1] = 0;
    }
    /* return tile bbox (without overlap) if asked to */
    if(tr) {
        *tr = bbox;
    }
    /* add suffix if any */
    if (suffix)
        strcat(ret,suffix);
    return tile;
}

void tile_bbox(char *tile, struct rect *r, int overlap) {
    struct coord c;
    int xo,yo;
    *r=world_bbox;
    while (*tile) {
        c.x=(r->l.x+r->h.x)/2;
        c.y=(r->l.y+r->h.y)/2;
        xo=(r->h.x-r->l.x)*overlap/100;
        yo=(r->h.y-r->l.y)*overlap/100;
        switch (*tile) {
        case 'a':
            if(*(tile +1)) {
                r->l.x=c.x;
                r->l.y=c.y;
            } else {
                r->l.x=c.x-xo;
                r->l.y=c.y-yo;
            }
            break;
        case 'b':
            if(*(tile +1)) {
                r->h.x=c.x;
                r->l.y=c.y;
            } else {
                r->h.x=c.x+xo;
                r->l.y=c.y-yo;
            }
            break;
        case 'c':
            if(*(tile +1)) {
                r->l.x=c.x;
                r->h.y=c.y;
            } else {
                r->l.x=c.x-xo;
                r->h.y=c.y+yo;
            }
            break;
        case 'd':
            if(*(tile +1)) {
                r->h.x=c.x;
                r->h.y=c.y;
            } else {
                r->h.x=c.x+xo;
                r->h.y=c.y+yo;
                break;
            }
        }
        tile++;
    }
}

int tile_len(char *tile) {
    int ret=0;
    while (tile[0] >= 'a' && tile[0] <= 'd') {
        tile++;
        ret++;
    }
    return ret;
}

static void tile_extend(char *tile, struct item_bin *ib, GList **tiles_list) {
    struct tile_head *th=NULL;
    if (debug_tile(tile))
        fprintf(stderr,"Tile:Writing %d bytes to '%s' (%p,%p) 0x%x "LONGLONG_FMT"\n", (ib->len+1)*4, tile,
                g_hash_table_lookup(tile_hash, tile), tile_hash2 ? g_hash_table_lookup(tile_hash2, tile) : NULL, ib->type,
                item_bin_get_id(ib));
    if (tile_hash2)
        th=g_hash_table_lookup(tile_hash2, tile);
    if (!th)
        th=g_hash_table_lookup(tile_hash, tile);
    if (! th) {
        th=g_malloc(sizeof(struct tile_head)+ sizeof( char* ) );
        // strcpy(th->subtiles, tile);
        th->num_subtiles=1;
        th->total_size=0;
        th->total_size_used=0;
        th->zipnum=0;
        th->zip_data=NULL;
        th->name=string_hash_lookup(tile);
        *th_get_subtile( th, 0 ) = th->name;

        if (tile_hash2)
            g_hash_table_insert(tile_hash2, string_hash_lookup( th->name ), th);
        if (tiles_list)
            *tiles_list=g_list_append(*tiles_list, string_hash_lookup( th->name ) );
        processed_tiles++;
        if (debug_tile(tile))
            fprintf(stderr,"new '%s'\n", tile);
    }
    th->total_size+=ib->len*4+4;
    if (debug_tile(tile))
        fprintf(stderr,"New total size of %s(%p):%d\n", th->name, th, th->total_size);
    g_hash_table_insert(tile_hash, string_hash_lookup( th->name ), th);
}

static int tile_data_size(char *tile) {
    struct tile_head *th;
    th=g_hash_table_lookup(tile_hash, tile);
    if (! th)
        return 0;
    return th->total_size;
}

static int merge_tile(char *base, char *sub) {
    struct tile_head *thb, *ths;
    thb=g_hash_table_lookup(tile_hash, base);
    ths=g_hash_table_lookup(tile_hash, sub);
    if (! ths)
        return 0;
    if (debug_tile(base) || debug_tile(sub))
        fprintf(stderr,"merging '%s'(%p) (%d) with '%s'(%p) (%d)\n", base, thb, thb ? thb->total_size : 0, sub, ths,
                ths->total_size);
    if (! thb) {
        thb=ths;
        g_hash_table_remove(tile_hash, sub);
        thb->name=string_hash_lookup(base);
        g_hash_table_insert(tile_hash, string_hash_lookup( thb->name ), thb);

    } else {
        thb=g_realloc(thb, sizeof(struct tile_head)+( ths->num_subtiles+thb->num_subtiles ) * sizeof( char*) );
        memcpy( th_get_subtile( thb, thb->num_subtiles ), th_get_subtile( ths, 0 ), ths->num_subtiles * sizeof( char*) );
        thb->num_subtiles+=ths->num_subtiles;
        thb->total_size+=ths->total_size;
        g_hash_table_insert(tile_hash, string_hash_lookup( thb->name ), thb);
        g_hash_table_remove(tile_hash, sub);
        g_free(ths);
    }
    return 1;
}

static gint get_tiles_list_cmp(gconstpointer s1, gconstpointer s2) {
    return g_strcmp0((char *)s1, (char *)s2);
}

static void get_tiles_list_func(char *key, struct tile_head *th, GList **list) {
    *list=g_list_prepend(*list, key);
}

static GList *get_tiles_list(void) {
    GList *ret=NULL;
    g_hash_table_foreach(tile_hash, (GHFunc)get_tiles_list_func, &ret);
    ret=g_list_sort(ret, get_tiles_list_cmp);
    return ret;
}

#if 0
static void write_tile(char *key, struct tile_head *th, gpointer dummy) {
    FILE *f;
    char buffer[1024];
    fprintf(stderr,"DEBUG: Writing %s\n", key);
    strcpy(buffer,"tiles/");
    strcat(buffer,key);
#if 0
    strcat(buffer,".bin");
#endif
    f=fopen(buffer, "wb+");
    while (th) {
        fwrite(th->data, th->size, 1, f);
        th=th->next;
    }
    fclose(f);
}
#endif

static void write_item(char *tile, struct item_bin *ib, FILE *reference) {
    struct tile_head *th;
    int size;

    th=g_hash_table_lookup(tile_hash2, tile);
    if (debug_itembin(ib)) {
        fprintf(stderr,"tile head %p\n",th);
    }
    if (! th)
        th=g_hash_table_lookup(tile_hash, tile);
    if (th) {
        if (debug_itembin(ib)) {
            fprintf(stderr,"Match %s %d %s\n",tile,th->process,th->name);
            dump_itembin(ib);
        }
        if (th->process != 0 && th->process != 1) {
            fprintf(stderr,"error with tile '%s' of length %d\n", tile, (int)strlen(tile));
            abort();
        }
        if (! th->process) {
            if (reference)
                fseek(reference, 8, SEEK_CUR);
            return;
        }
        if (debug_tile(tile))
            fprintf(stderr,"Data:Writing %d bytes to '%s' (%p,%p) 0x%x\n", (ib->len+1)*4, tile, g_hash_table_lookup(tile_hash,
                    tile), tile_hash2 ? g_hash_table_lookup(tile_hash2, tile) : NULL, ib->type);
        size=(ib->len+1)*4;
        if (th->total_size_used+size > th->total_size) {
            fprintf(stderr,"Overflow in tile %s (used %d max %d item %d)\n", tile, th->total_size_used, th->total_size, size);
            exit(1);
            return;
        }
        if (reference) {
            int offset=th->total_size_used/4;
            dbg_assert(fwrite(&th->zipnum, sizeof(th->zipnum), 1, reference)==1);
            dbg_assert(fwrite(&offset, sizeof(th->total_size_used), 1, reference)==1);
        }
        if (th->zip_data)
            memcpy(th->zip_data+th->total_size_used, ib, size);
        th->total_size_used+=size;
    } else {
        fprintf(stderr,"no tile hash found for %s\n", tile);
        exit(1);
    }
}

void tile_write_item_to_tile(struct tile_info *info, struct item_bin *ib, FILE *reference, char *name) {
    if (info->write)
        write_item(name, ib, reference);
    else
        tile_extend(name, ib, info->tiles_list);
}

void tile_write_item_minmax(struct tile_info *info, struct item_bin *ib, FILE *reference, int min, int max) {
    /*TODO: make slice_trigger and slice_target configurable by commandline parameter.
     * bonus: find out why there is a 'min' parameter here
     */
    int slice_trigger = 4;
    int slice_target = 7;
    struct rect r;
    char buffer[1024];
    bbox((struct coord *)(ib+1), ib->clen/2, &r);
    buffer[0]='\0';
    tile(&r, info->suffix, buffer, max, overlap, NULL);
    if((ib->type >= type_area) && (ib->type != type_poly_water_tiled) && (tile_len(buffer) < slice_trigger)) {
        itembin_nicer_slicer(info, ib, reference, buffer, slice_target);
    } else {
        tile_write_item_to_tile(info, ib, reference, buffer);
    }
}

int add_aux_tile(struct zip_info *zip_info, char *name, char *filename, int size) {
    struct aux_tile *at;
    GList *l;
    l=aux_tile_list;
    while (l) {
        at=l->data;
        if (!g_strcmp0(at->name, name)) {
            return -1;
        }
        l=g_list_next(l);
    }
    at=g_new0(struct aux_tile, 1);
    at->name=g_strdup(name);
    at->filename=g_strdup(filename);
    at->size=size;
    aux_tile_list=g_list_append(aux_tile_list, at);
    fprintf(stderr,"Adding %s as %s\n",filename, name);
    return zip_add_member(zip_info);
}

int write_aux_tiles(struct zip_info *zip_info) {
    GList *l=aux_tile_list;
    struct aux_tile *at;
    char *buffer;
    FILE *f;
    int count=0;

    while (l) {
        at=l->data;
        buffer=g_malloc(at->size);
        f=fopen(at->filename,"rb");
        assert(f != NULL);

        if (fread(buffer, at->size, 1, f) == 0) {
            dbg(lvl_warning, "fread failed");
            fclose(f);
        } else {
            fclose(f);
            write_zipmember(zip_info, at->name, zip_get_maxnamelen(zip_info), buffer, at->size);
            count++;
            l=g_list_next(l);
            zip_add_member(zip_info);
        }
        g_free(buffer);
    }
    return count;
}

static int add_tile_hash(struct tile_head *th) {
    int idx,len,maxnamelen=0;
    char **data;

    for( idx = 0; idx < th->num_subtiles; idx++ ) {

        data = th_get_subtile( th, idx );

        if (debug_tile(((char *)data)) || debug_tile(th->name)) {
            fprintf(stderr,"Parent for '%s' is '%s'\n", *data, th->name);
        }

        g_hash_table_insert(tile_hash2, *data, th);

        len = strlen( *data );

        if (len > maxnamelen) {
            maxnamelen=len;
        }
    }
    return maxnamelen;
}


int create_tile_hash(void) {
    struct tile_head *th;
    int len,maxnamelen=0;

    tile_hash2=g_hash_table_new(g_str_hash, g_str_equal);
    th=tile_head_root;
    while (th) {
        len=add_tile_hash(th);
        if (len > maxnamelen)
            maxnamelen=len;
        th=th->next;
    }
    return maxnamelen;
}

static void create_tile_hash_list(GList *list) {
    GList *next;
    struct tile_head *th;

    tile_hash2=g_hash_table_new(g_str_hash, g_str_equal);

    next=g_list_first(list);
    while (next) {
        th=g_hash_table_lookup(tile_hash, next->data);
        if (!th) {
            fprintf(stderr,"No tile found for '%s'\n", (char *)(next->data));
        }
        add_tile_hash(th);
        next=g_list_next(next);
    }
}

void load_tilesdir(FILE *in) {
    char tile[32],subtile[32],c;
    int size,zipnum=0;
    struct tile_head **last;
    create_tile_hash();
    tile_hash=g_hash_table_new(g_str_hash, g_str_equal);
    last=&tile_head_root;
    while (fscanf(in,"%[^:]:%d",tile,&size) == 2) {
        struct tile_head *th=g_malloc(sizeof(struct tile_head));
        if (!g_strcmp0(tile,"index"))
            tile[0]='\0';
        th->num_subtiles=0;
        th->total_size=size;
        th->total_size_used=0;
        th->zipnum=zipnum++;
        th->zip_data=NULL;
        th->name=string_hash_lookup(tile);
        while (fscanf(in,":%[^:\n]",subtile) == 1) {
            th=g_realloc(th, sizeof(struct tile_head)+(th->num_subtiles+1)*sizeof(char*));
            *th_get_subtile( th, th->num_subtiles ) = string_hash_lookup(subtile);
            th->num_subtiles++;
        }
        *last=th;
        last=&th->next;
        add_tile_hash(th);
        g_hash_table_insert(tile_hash, th->name, th);
        if (fread(&c, 1, 1, in) != 1 || c != '\n') {
            printf("syntax error\n");
        }
    }
    *last=NULL;
}

void write_tilesdir(struct tile_info *info, struct zip_info *zip_info, FILE *out) {
    int idx,len,maxlen;
    GList *next,*tiles_list;
    char **data;
    struct tile_head *th,**last=NULL;

    tiles_list=get_tiles_list();
    info->tiles_list=&tiles_list;
    if (! info->write)
        create_tile_hash_list(tiles_list);
    next=g_list_first(tiles_list);
    last=&tile_head_root;
    maxlen=info->maxlen;
    if (! maxlen) {
        while (next) {
            if (strlen(next->data) > maxlen)
                maxlen=strlen(next->data);
            next=g_list_next(next);
        }
    }
    len=maxlen;
    while (len >= 0) {
        next=g_list_first(tiles_list);
        while (next) {
            if (strlen(next->data) == len) {
                th=g_hash_table_lookup(tile_hash, next->data);
                if (!info->write) {
                    *last=th;
                    last=&th->next;
                    th->next=NULL;
                    th->zipnum=zip_get_zipnum(zip_info);
                    fprintf(out,"%s:%d",strlen((char *)next->data)?(char *)next->data:"index",th->total_size);

                    for ( idx = 0; idx< th->num_subtiles; idx++ ) {
                        data= th_get_subtile( th, idx );
                        fprintf(out,":%s", *data);
                    }

                    fprintf(out,"\n");
                }
                if (th->name[strlen(info->suffix)])
                    index_submap_add(info, th);
                zip_add_member(zip_info);
                processed_tiles++;
            }
            next=g_list_next(next);
        }
        len--;
    }
    g_list_free(tiles_list);
    if (info->suffix[0] && info->write) {
        struct item_bin *item_bin=init_item(type_submap);
        item_bin_add_coord_rect(item_bin, &world_bbox);
        item_bin_add_attr_range(item_bin, attr_order, 0, 255);
        item_bin_add_attr_int(item_bin, attr_zipfile_ref, zip_get_zipnum(zip_info)-1);
        item_bin_write(item_bin, zip_get_index(zip_info));
    }
}

void merge_tiles(struct tile_info *info) {
    struct tile_head *th;
    char basetile[1024];
    char subtile[1024];
    GList *tiles_list_sorted,*last;
    int i,i_min,len,size_all,size[5],size_min,work_done;
    long long zip_size;

    do {
        tiles_list_sorted=get_tiles_list();
        fprintf(stderr,"PROGRESS: sorting %d tiles\n", g_list_length(tiles_list_sorted));
        tiles_list_sorted=g_list_sort(tiles_list_sorted, (GCompareFunc)g_strcmp0);
        fprintf(stderr,"PROGRESS: sorting %d tiles done\n", g_list_length(tiles_list_sorted));
        last=g_list_last(tiles_list_sorted);
        zip_size=0;
        while (last) {
            th=g_hash_table_lookup(tile_hash, last->data);
            zip_size+=th->total_size;
            last=g_list_previous(last);
        }
        last=g_list_last(tiles_list_sorted);
        work_done=0;
        while (last) {
            processed_tiles++;
            len=tile_len(last->data);
            if (len >= 1) {
                strcpy(basetile,last->data);
                basetile[len-1]='\0';
                strcat(basetile, info->suffix);
                strcpy(subtile,last->data);
                for (i = 0 ; i < 4 ; i++) {
                    subtile[len-1]='a'+i;
                    size[i]=tile_data_size(subtile);
                }
                size[4]=tile_data_size(basetile);
                size_all=size[0]+size[1]+size[2]+size[3]+size[4];
                if (size_all < 65536 && size_all > 0 && size_all != size[4]) {
                    for (i = 0 ; i < 4 ; i++) {
                        subtile[len-1]='a'+i;
                        work_done+=merge_tile(basetile, subtile);
                    }
                } else {
                    for (;;) {
                        size_min=size_all;
                        i_min=-1;
                        for (i = 0 ; i < 4 ; i++) {
                            if (size[i] && size[i] < size_min) {
                                size_min=size[i];
                                i_min=i;
                            }
                        }
                        if (i_min == -1)
                            break;
                        if (size[4]+size_min >= 65536)
                            break;
                        subtile[len-1]='a'+i_min;
                        work_done+=merge_tile(basetile, subtile);
                        size[4]+=size[i_min];
                        size[i_min]=0;
                    }
                }
            }
            last=g_list_previous(last);
        }
        g_list_free(tiles_list_sorted);
        fprintf(stderr,"PROGRESS: merged %d tiles\n", work_done);
    } while (work_done);
}

struct attr map_information_attrs[32];

void index_init(struct zip_info *info, int version) {
    struct item_bin *item_bin;
    int i;
    map_information_attrs[0].type=attr_version;
    map_information_attrs[0].u.num=version;
    item_bin=init_item(type_map_information);
    for (i = 0 ; i < 32 ; i++) {
        if (!map_information_attrs[i].type)
            break;
        item_bin_add_attr(item_bin, &map_information_attrs[i]);
    }
    item_bin_write(item_bin, zip_get_index(info));
}

void index_submap_add(struct tile_info *info, struct tile_head *th) {
    int tlen=tile_len(th->name);
    int len=tlen;
    char *index_tile;
    struct rect r;
    struct item_bin *item_bin;

    index_tile=g_alloca(len+1+strlen(info->suffix));
    strcpy(index_tile, th->name);
    if (len > 6)
        len=6;
    else
        len=0;
    index_tile[len]=0;
    strcat(index_tile, info->suffix);
    tile_bbox(th->name, &r, overlap);

    item_bin=init_item(type_submap);
    item_bin_add_coord_rect(item_bin, &r);
    item_bin_add_attr_range(item_bin, attr_order, (tlen > 4)?tlen-4 : 0, 255);
    item_bin_add_attr_int(item_bin, attr_zipfile_ref, th->zipnum);
    tile_write_item_to_tile(info, item_bin, NULL, index_tile);
}
