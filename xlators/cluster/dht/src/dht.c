/*
  Copyright (c) 2008-2010 Gluster, Inc. <http://www.gluster.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/


#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

/* TODO: add NS locking */

#include "statedump.h"
#include "dht-common.c"

/* TODO:
   - use volumename in xattr instead of "dht"
   - use NS locks
   - handle all cases in self heal layout reconstruction
   - complete linkfile selfheal
*/


void
dht_layout_dump (dht_layout_t  *layout, const char *prefix)
{

        char    key[GF_DUMP_MAX_BUF_LEN];
        int     i = 0;

        GF_VALIDATE_OR_GOTO ("dht", layout, out);
        GF_VALIDATE_OR_GOTO ("dht", prefix, out);

        gf_proc_dump_build_key(key, prefix, "cnt");
        gf_proc_dump_write(key, "%d", layout->cnt);
        gf_proc_dump_build_key(key, prefix, "preset");
        gf_proc_dump_write(key, "%d", layout->preset);
        gf_proc_dump_build_key(key, prefix, "gen");
        gf_proc_dump_write(key, "%d", layout->gen);
        gf_proc_dump_build_key(key, prefix, "type");
        gf_proc_dump_write(key, "%d", layout->type);

        for (i = 0; i < layout->cnt; i++) {
                gf_proc_dump_build_key(key, prefix,"list[%d].err", i);
                gf_proc_dump_write(key, "%d", layout->list[i].err);
                gf_proc_dump_build_key(key, prefix,"list[%d].start", i);
                gf_proc_dump_write(key, "%u", layout->list[i].start);
                gf_proc_dump_build_key(key, prefix,"list[%d].stop", i);
                gf_proc_dump_write(key, "%u", layout->list[i].stop);
                if (layout->list[i].xlator) {
                        gf_proc_dump_build_key(key, prefix,
                                               "list[%d].xlator.type", i);
                        gf_proc_dump_write(key, "%s",
                                           layout->list[i].xlator->type);
                        gf_proc_dump_build_key(key, prefix,
                                               "list[%d].xlator.name", i);
                        gf_proc_dump_write(key, "%s",
                                           layout->list[i].xlator->name);
                }
        }

out:
        return;
}


int32_t
dht_priv_dump (xlator_t *this)
{
        char            key_prefix[GF_DUMP_MAX_BUF_LEN];
        char            key[GF_DUMP_MAX_BUF_LEN];
        int             i = 0;
        dht_conf_t      *conf = NULL;
        int             ret = -1;

        GF_VALIDATE_OR_GOTO ("dht", this, out);

        conf = this->private;

        if (!conf)
                return -1;

        ret = TRY_LOCK(&conf->subvolume_lock);

        if (ret != 0) {
                gf_log("", GF_LOG_WARNING, "Unable to lock dht subvolume %s",
                       this->name);
                return ret;
        }

        gf_proc_dump_add_section("xlator.cluster.dht.%s.priv", this->name);
        gf_proc_dump_build_key(key_prefix,"xlator.cluster.dht","%s.priv",
                               this->name);
        gf_proc_dump_build_key(key, key_prefix, "subvolume_cnt");
        gf_proc_dump_write(key,"%d", conf->subvolume_cnt);
        for (i = 0; i < conf->subvolume_cnt; i++) {
                gf_proc_dump_build_key(key, key_prefix, "subvolumes[%d]", i);
                gf_proc_dump_write(key, "%s.%s", conf->subvolumes[i]->type,
                                   conf->subvolumes[i]->name);
                if (conf->file_layouts && conf->file_layouts[i]){
                        gf_proc_dump_build_key(key, key_prefix,
                                               "file_layouts[%d]",i);
                        dht_layout_dump(conf->file_layouts[i], key);
                }
                if (conf->dir_layouts && conf->dir_layouts[i]) {
                        gf_proc_dump_build_key(key, key_prefix,
                                               "dir_layouts[%d]",i);
                        dht_layout_dump(conf->dir_layouts[i], key);
                }
                if (conf->subvolume_status) {
                        gf_proc_dump_build_key(key, key_prefix,
                                               "subvolume_status[%d]", i);
                        gf_proc_dump_write(key, "%d",
                                           (int)conf->subvolume_status[i]);
                }

        }

        gf_proc_dump_build_key(key, key_prefix,"default_dir_layout");
        dht_layout_dump(conf->default_dir_layout, key);

        gf_proc_dump_build_key(key, key_prefix, "search_unhashed");
        gf_proc_dump_write(key, "%d", conf->search_unhashed);
        gf_proc_dump_build_key(key, key_prefix, "gen");
        gf_proc_dump_write(key, "%d", conf->gen);
        gf_proc_dump_build_key(key, key_prefix, "min_free_disk");
        gf_proc_dump_write(key, "%lu", conf->min_free_disk);
        gf_proc_dump_build_key(key, key_prefix, "disk_unit");
        gf_proc_dump_write(key, "%c", conf->disk_unit);
        gf_proc_dump_build_key(key, key_prefix, "refresh_interval");
        gf_proc_dump_write(key, "%d", conf->refresh_interval);
        gf_proc_dump_build_key(key, key_prefix, "unhashed_sticky_bit");
        gf_proc_dump_write(key, "%d", conf->unhashed_sticky_bit);
        if (conf ->du_stats) {
                gf_proc_dump_build_key(key, key_prefix,
                                       "du_stats.avail_percent");
                gf_proc_dump_write(key, "%lf", conf->du_stats->avail_percent);
                gf_proc_dump_build_key(key, key_prefix,
                                       "du_stats.avail_space");
                gf_proc_dump_write(key, "%lu", conf->du_stats->avail_space);
                gf_proc_dump_build_key(key, key_prefix,
                                       "du_stats.log");
                gf_proc_dump_write(key, "%lu", conf->du_stats->log);
        }
        gf_proc_dump_build_key(key, key_prefix, "last_stat_fetch");
        gf_proc_dump_write(key, "%s", ctime(&conf->last_stat_fetch.tv_sec));

        UNLOCK(&conf->subvolume_lock);

out:
        return ret;
}

int32_t
dht_inodectx_dump (xlator_t *this, inode_t *inode)
{
        int             ret = -1;
        char            key_prefix[GF_DUMP_MAX_BUF_LEN];
        dht_layout_t    *layout = NULL;
        uint64_t        tmp_layout = 0;

        GF_VALIDATE_OR_GOTO ("dht", this, out);
        GF_VALIDATE_OR_GOTO ("dht", inode, out);

        ret = inode_ctx_get (inode, this, &tmp_layout);

        if (ret != 0)
                return ret;

        layout = (dht_layout_t *)(long)tmp_layout;

        if (!layout)
                return -1;

        gf_proc_dump_build_key(key_prefix, "xlator.cluster.dht",
                               "%s.inode.%ld", this->name, inode->ino);
        dht_layout_dump(layout, key_prefix);

out:
        return ret;
}

int
notify (xlator_t *this, int event, void *data, ...)
{
        int ret = -1;

        GF_VALIDATE_OR_GOTO ("dht", this, out);

        ret = dht_notify (this, event, data);

out:
        return ret;
}

void
fini (xlator_t *this)
{
        int         i = 0;
        dht_conf_t *conf = NULL;

        GF_VALIDATE_OR_GOTO ("dht", this, out);

        conf = this->private;
        this->private = NULL;
        if (conf) {
                if (conf->file_layouts) {
                        for (i = 0; i < conf->subvolume_cnt; i++) {
                                GF_FREE (conf->file_layouts[i]);
                        }
                        GF_FREE (conf->file_layouts);
                }

                if (conf->default_dir_layout)
                        GF_FREE (conf->default_dir_layout);

                if (conf->subvolumes)
                        GF_FREE (conf->subvolumes);

                if (conf->subvolume_status)
                        GF_FREE (conf->subvolume_status);

                GF_FREE (conf);
        }
out:
        return;
}

int32_t
mem_acct_init (xlator_t *this)
{
        int     ret = -1;

        GF_VALIDATE_OR_GOTO ("dht", this, out);

        ret = xlator_mem_acct_init (this, gf_dht_mt_end + 1);

        if (ret != 0) {
                gf_log (this->name, GF_LOG_ERROR, "Memory accounting init"
                        "failed");
                return ret;
        }
out:
        return ret;
}

int
validate_options (xlator_t *this, char **op_errstr)
{
        int                 ret = 0;
        volume_opt_list_t  *vol_opt = NULL;
        volume_opt_list_t  *tmp;

        if (!this) {
                gf_log (this->name, GF_LOG_DEBUG, "'this' not a valid ptr");
                ret =-1;
                goto out;
        }

        if (list_empty (&this->volume_options))
                goto out;

        vol_opt = list_entry (this->volume_options.next,
                                      volume_opt_list_t, list);
         list_for_each_entry_safe (vol_opt, tmp, &this->volume_options, list) {
                ret = validate_xlator_volume_options_attacherr (this,
                                                                vol_opt->given_opt,
                                                                op_errstr);
        }

out:

        return ret;
}

int
reconfigure (xlator_t *this, dict_t *options)
{
        dht_conf_t      *conf = NULL;
        char            *temp_str = NULL;
        gf_boolean_t     search_unhashed;
        uint32_t         temp_free_disk = 0;
        int              ret = -1;

        GF_VALIDATE_OR_GOTO ("dht", this, out);
        GF_VALIDATE_OR_GOTO ("dht", options, out);

        conf = this->private;
        if (!conf)
                return 0;

        if (dict_get_str (options, "lookup-unhashed", &temp_str) == 0) {
                /* If option is not "auto", other options _should_ be boolean*/
                if (strcasecmp (temp_str, "auto")) {
                        if (!gf_string2boolean (temp_str, &search_unhashed)) {
                                gf_log(this->name, GF_LOG_DEBUG, "Reconfigure:"
                                       " lookup-unahashed reconfigured (%s)",
                                       temp_str);
                                conf->search_unhashed = search_unhashed;
                        } else {
                                gf_log(this->name, GF_LOG_ERROR, "Reconfigure:"
                                       " lookup-unahashed should be boolean,"
                                       " not (%s), defaulting to (%d)",
                                       temp_str, conf->search_unhashed);
                                //return -1;
                                ret = -1;
                                goto out;
                        }
                } else {
                        gf_log(this->name, GF_LOG_DEBUG, "Reconfigure:"
                               " lookup-unahashed reconfigured auto ");
                        conf->search_unhashed = GF_DHT_LOOKUP_UNHASHED_AUTO;
                }
        }

        if (dict_get_str (options, "min-free-disk", &temp_str) == 0) {
                if (gf_string2percent (temp_str, &temp_free_disk) == 0) {
                        if (temp_free_disk > 100) {
                                gf_string2bytesize (temp_str,
                                                    &conf->min_free_disk);
                                conf->disk_unit = 'b';
                        } else {
                                conf->min_free_disk = (uint64_t)temp_free_disk;
                        }
                } else {
                        gf_string2bytesize (temp_str, &conf->min_free_disk);
                        conf->disk_unit = 'b';
                }

                gf_log(this->name, GF_LOG_DEBUG, "Reconfigure:"
                       " min-free-disk reconfigured to %s",
                       temp_str);
        }
        ret = 0;
out:
        return ret;
}

int
init (xlator_t *this)
{
        dht_conf_t    *conf = NULL;
        char          *temp_str = NULL;
        int            ret = -1;
        int            i = 0;
        uint32_t       temp_free_disk = 0;

        GF_VALIDATE_OR_GOTO ("dht", this, err);

        if (!this->children) {
                gf_log (this->name, GF_LOG_CRITICAL,
                        "Distribute needs more than one subvolume");
                return -1;
        }

        if (!this->parents) {
                gf_log (this->name, GF_LOG_WARNING,
                        "dangling volume. check volfile");
        }

        conf = GF_CALLOC (1, sizeof (*conf), gf_dht_mt_dht_conf_t);
        if (!conf) {
                goto err;
        }

        conf->search_unhashed = GF_DHT_LOOKUP_UNHASHED_ON;
        if (dict_get_str (this->options, "lookup-unhashed", &temp_str) == 0) {
                /* If option is not "auto", other options _should_ be boolean */
                if (strcasecmp (temp_str, "auto"))
                        gf_string2boolean (temp_str, &conf->search_unhashed);
                else
                        conf->search_unhashed = GF_DHT_LOOKUP_UNHASHED_AUTO;
        }

        conf->unhashed_sticky_bit = 0;

        if (dict_get_str (this->options, "unhashed-sticky-bit",
                          &temp_str) == 0) {
                gf_string2boolean (temp_str, &conf->unhashed_sticky_bit);
        }

        conf->use_readdirp = 1;

        if (dict_get_str (this->options, "use-readdirp",
                          &temp_str) == 0) {
                gf_string2boolean (temp_str, &conf->use_readdirp);
        }

        conf->disk_unit = 'p';
        conf->min_free_disk = 10;

        if (dict_get_str (this->options, "min-free-disk", &temp_str) == 0) {
                if (gf_string2percent (temp_str, &temp_free_disk) == 0) {
                        if (temp_free_disk > 100) {
                                gf_string2bytesize (temp_str,
                                                    &conf->min_free_disk);
                                conf->disk_unit = 'b';
                        } else {
                                conf->min_free_disk = (uint64_t)temp_free_disk;
                        }
                } else {
                        gf_string2bytesize (temp_str, &conf->min_free_disk);
                        conf->disk_unit = 'b';
                }
        }

        conf->assert_no_child_down = 0;

        if (!dict_get_str (this->options, "assert-no-child-down", &temp_str)) {
                if (!strncmp (temp_str, "on", strlen (temp_str))) {
                        conf->assert_no_child_down = 1;
                        gf_log (this->name, GF_LOG_INFO,
                                "assert-no-child-down set to on");
                } else {
                        conf->assert_no_child_down = 0;
                        gf_log (this->name, GF_LOG_INFO,
                                "assert-no-child-down set to off");
                }
        }

        ret = dht_init_subvolumes (this, conf);
        if (ret == -1) {
                goto err;
        }

        ret = dht_layouts_init (this, conf);
        if (ret == -1) {
                goto err;
        }

        conf->du_stats = GF_CALLOC (conf->subvolume_cnt, sizeof (dht_du_t),
                                    gf_dht_mt_dht_du_t);
        if (!conf->du_stats) {
                goto err;
        }

        LOCK_INIT (&conf->subvolume_lock);
        LOCK_INIT (&conf->layout_lock);

        conf->gen = 1;

        this->private = conf;

        return 0;

err:
        if (conf) {
                if (conf->file_layouts) {
                        for (i = 0; i < conf->subvolume_cnt; i++) {
                                GF_FREE (conf->file_layouts[i]);
                        }
                        GF_FREE (conf->file_layouts);
                }

                if (conf->default_dir_layout)
                        GF_FREE (conf->default_dir_layout);

                if (conf->subvolumes)
                        GF_FREE (conf->subvolumes);

                if (conf->subvolume_status)
                        GF_FREE (conf->subvolume_status);

                if (conf->du_stats)
                        GF_FREE (conf->du_stats);

                GF_FREE (conf);
        }

        return -1;
}


struct xlator_fops fops = {
        .lookup      = dht_lookup,
        .mknod       = dht_mknod,
        .create      = dht_create,

        .stat        = dht_stat,
        .fstat       = dht_fstat,
        .truncate    = dht_truncate,
        .ftruncate   = dht_ftruncate,
        .access      = dht_access,
        .readlink    = dht_readlink,
        .setxattr    = dht_setxattr,
        .fsetxattr   = dht_fsetxattr,
        .getxattr    = dht_getxattr,
        .removexattr = dht_removexattr,
        .open        = dht_open,
        .readv       = dht_readv,
        .writev      = dht_writev,
        .flush       = dht_flush,
        .fsync       = dht_fsync,
        .statfs      = dht_statfs,
        .lk          = dht_lk,
        .opendir     = dht_opendir,
        .readdir     = dht_readdir,
        .readdirp    = dht_readdirp,
        .fsyncdir    = dht_fsyncdir,
        .symlink     = dht_symlink,
        .unlink      = dht_unlink,
        .link        = dht_link,
        .mkdir       = dht_mkdir,
        .rmdir       = dht_rmdir,
        .rename      = dht_rename,
        .inodelk     = dht_inodelk,
        .finodelk    = dht_finodelk,
        .entrylk     = dht_entrylk,
        .fentrylk    = dht_fentrylk,
        .xattrop     = dht_xattrop,
        .fxattrop    = dht_fxattrop,
        .setattr     = dht_setattr,
        .fsetattr    = dht_fsetattr,
};

struct xlator_dumpops dumpops = {
        .priv = dht_priv_dump,
        .inodectx = dht_inodectx_dump,
};


struct xlator_cbks cbks = {
//      .release    = dht_release,
//      .releasedir = dht_releasedir,
        .forget     = dht_forget
};


struct volume_options options[] = {
        { .key  = {"lookup-unhashed"},
          .value = {"auto", "yes", "no", "enable", "disable", "1", "0",
                    "on", "off"},
          .type = GF_OPTION_TYPE_STR
        },
        { .key  = {"min-free-disk"},
          .type = GF_OPTION_TYPE_PERCENT_OR_SIZET,
        },
        { .key = {"unhashed-sticky-bit"},
          .type = GF_OPTION_TYPE_BOOL
        },
        { .key = {"use-readdirp"},
          .type = GF_OPTION_TYPE_BOOL
        },
        { .key = {"assert-no-child-down"},
          .value = {"on", "off"},
          .type = GF_OPTION_TYPE_STR
        },
        { .key  = {NULL} },
};
