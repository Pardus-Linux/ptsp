#!/usr/bin/python
# -*- coding: utf-8 -*-
#
# Copyright (C) 2006, TUBITAK/UEKAE
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version. Please read the COPYING file.
#

import os
import shutil
import subprocess
import stat
import sys

def waitBus(unix_name, timeout=5, wait=0.1, stream=True):
    import socket
    import time
    if stream:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    else:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    while timeout > 0:
        try:
            sock.connect(unix_name)
            return True
        except:
            timeout -= wait
        time.sleep(wait)
    return False

def run(cmd, ignore_error=False):
    print cmd
    ret = os.system(cmd)
    if ret and not ignore_error:
        print "%s returned %s" % (cmd, ret)
        sys.exit(1)

def chroot_comar(image_dir):
    if os.fork() == 0:
        os.chroot(image_dir)
        subprocess.call(["/usr/bin/comar"])
        sys.exit(0)
    waitBus("%s/var/run/comar.socket" % image_dir)

def make_image():

    image_dir = "/home/faik/ptsp/ptsp-client-rootfs"

    # REMOVE
    repo_dir = "http://192.168.3.110/pardus-2007"

    # REMOVE
    # tmp ptsp repo contains lbuscd, ltspfsd, ptsp-client packages
    ptsp_dir = "/home/faik/ptsp/ptsprepo"

    try:
        run('pisi --yes-all -D"%s" ar pardus-2007 %s' % (image_dir, repo_dir + "/pisi-index.xml.bz2"))
        run('pisi --yes-all -D"%s" ar ptsp-2007 %s' % (image_dir, ptsp_dir + "/pisi-index.xml.bz2"))

        run('pisi --yes-all --ignore-comar --ignore-file-conflicts -D"%s" it -c system.base' % image_dir)
        run('pisi --yes-all --ignore-comar --ignore-file-conflicts -D"%s" it xorg-server' % image_dir)
        run('pisi --yes-all --ignore-comar --ignore-file-conflicts -D"%s" it ptsp-client' % image_dir)
        
        def chrun(cmd):
            run('chroot "%s" %s' % (image_dir, cmd))
        
        path = "%s/usr/share/baselayout/" % image_dir
        path2 = "%s/etc" % image_dir
        for name in os.listdir(path):
            run('cp -p "%s" "%s"' % (os.path.join(path, name), os.path.join(path2, name)))
        run('/bin/mount --bind /proc %s/proc' % image_dir)
        run('/bin/mount --bind /sys %s/sys' % image_dir)
        
        chrun("/sbin/ldconfig")
        chrun("/sbin/update-environment")
        chroot_comar(image_dir)
        chrun("/usr/bin/hav call-package System.Package.postInstall baselayout")
        chrun("/usr/bin/pisi configure-pending")
        
        chrun("hav call User.Manager.setUser uid 0 password pardus")

        chrun("/usr/bin/comar --stop")
        
        chrun("/sbin/update-modules")

        # FIX
        chrun("/sbin/depmod -a kernel-2.6.18.8-83")
        
        file(os.path.join(image_dir, "etc/pardus-release"), "w").write("Pardus 2007\n")

        chrun("/usr/bin/pisi dc")

        # devices will be created in postinstall of ptsp-server
        os.unlink("%s/dev/null" % image_dir)
        shutil.rmtree("%s/lib/udev/devices" % image_dir)

        run('umount %s/proc' % image_dir)
        run('umount %s/sys' % image_dir)
    except KeyboardInterrupt:
        run('umount %s/proc' % image_dir, ignore_error=True)
        run('umount %s/sys' % image_dir, ignore_error=True)
        sys.exit(1)

if __name__ == "__main__":
    make_image()
    
