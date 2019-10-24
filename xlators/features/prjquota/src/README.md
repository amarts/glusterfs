# Project Quota

This implementation of quota is very specific to kadalu usecase, ie., used
in subdirectory specific quota. Where each subdirectory would remain as a
separate entity, and renames, links across the subdirs, upon which quota is
set would be not allowed.

## Implementation Details

This Translator follows a simple logic where, a directory, upon which Quota
is set, would be sharing the context with all its child entities. Only limit
is set on brick as xattr. All other data can be populated on the run by
looking up recursively (for now). In future, a mechanism where one can store
the latest info also on disk is a possibility.


Set the limit by doing:

```
# setfattr -n prjquota.limit -v 1024000000 /mnt/glusterfs/directory/
# df -h /mnt/glusterfs/directory/

```

TODO: Convert value from human readable to value.

## Volume file template
```
volume prjquota
  type features/prjquota
  options config-file {{ brick-dir }}/.glusterfs/prjquota-config.txt
  subdir <anything-above-posix>
end-volume
```

This should be ideally placed in brick graph.