#!/bin/sh
rm -f sources.tar
git -c "tar.umask=0022" archive --format=tar HEAD > sources.tar
git submodule foreach --recursive "git -c \"tar.umask=0022\" archive --format=tar --prefix=\$path/ --output=$PWD/\$sha1.tar HEAD && tar --concatenate --file=$PWD/sources.tar $PWD/\$sha1.tar; rm $PWD/\$sha1.tar"
       