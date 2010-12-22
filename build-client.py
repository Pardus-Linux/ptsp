#!/usr/bin/python
# -*- coding: utf-8 -*-
#
# Copyright (C) 2007, TUBITAK/UEKAE
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version. Please read the COPYING file.
#

import os
import sys
import glob
import stat
import time
import getopt
import shutil
import socket
import subprocess

default_exclude_list = """
lib/rcscripts/
usr/include/
usr/lib/cups/
usr/lib/python2.6/lib-tk/
usr/lib/python2.6/bsddb/test/
usr/lib/python2.6/lib-old/
usr/lib/python2.6/test/
usr/lib/klibc/include/
usr/qt/4/include/
usr/qt/4/mkspecs/
usr/qt/4/bin/
usr/share/aclocal/
usr/share/cups/
usr/share/doc/
usr/share/info/
usr/share/sip/
usr/share/man/
var/db/pisi/
var/cache/pisi
var/lib/pisi/
tmp/pisi-root/
var/log/pisi.log
root/.bash_history
"""

# Remove *.pyc files under /var/db/comar3/scripts as they prevent starting of system services
default_glob_excludes = (
    ( "var/db/comar3/", "*.pyc"),
    ( "usr/lib/python2.6/", "*.pyc" ),
    ( "usr/lib/python2.6/", "*.pyo" ),
    ( "usr/lib/", "*.a" ),
    ( "usr/lib/", "*.la" ),
    ( "lib/", "*.a" ),
    ( "lib/", "*.la" ),
)


PACKAGES = """
lbuscd
ltspfsd
ptsp-client
xorg-server
zorg
acpid
module-alsa-driver
alsa-plugins-pulseaudio
alsa-plugins
xorg-font
"""

# Install x11 drivers and hardware firmwares with system base components
COMPONENTS = ["system.base", "x11.driver"]

# Exclude proprietary drivers
PACKAGE_EXCLUDES = ["*-video", "xorg-video-fglrx", "xorg-video-nvidia*"]

def chroot_comar(image_dir):
    if os.fork() == 0:
        try:
            os.makedirs(os.path.join(image_dir, "var/db"), 0700)
        except OSError:
            pass
        os.chroot(image_dir)
        if not os.path.exists("/var/lib/dbus/machine-id"):
            run("/usr/bin/dbus-uuidgen --ensure")
        run("/sbin/start-stop-daemon -b --start --pidfile /var/run/dbus/pid --exec /usr/bin/dbus-daemon -- --system")
        sys.exit(0)

    # wait comar to start
    timeout = 5
    wait = 0.1
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    while timeout > 0:
        try:
            sock.connect("%s/var/run/dbus/system_bus_socket" % image_dir)
            return True
        except:
            timeout -= wait
        time.sleep(wait)
    return False

# run command and terminate if something goes wrong
def run(cmd, ignore_error=False):
    print cmd
    ret = os.system(cmd)
    if ret and not ignore_error:
        print "%s returned %s" % (cmd, ret)
        sys.exit(1)

def chroot_call(image_dir, func, *args):
    if os.fork() == 0:
        try:
            os.chroot(image_dir)
            func()
        except OSError:
            pass
        sys.exit(0)

def set_root_password():
    import comar

    link = comar.Link()
    link.User.Manager["baselayout"].setUser(0, "Root", "/root", "/bin/bash/", "pardus", [])

def make_initramfs():
    import glob

    kernel_image = glob.glob1("/boot", "kernel-*")[0]
    suffix = kernel_image.split("-", 1)[1]
    os.system("/sbin/mkinitramfs --network --kernel=%s" % suffix)

def get_exclude_list(output_dir):
    import fnmatch

    temp = default_exclude_list.split()
    for exc in default_glob_excludes:
        path = os.path.join(output_dir, exc[0])
        for root, dirs, files in os.walk(path):
            for name in files:
                if fnmatch.fnmatch(name, exc[1]):
                    temp.append(os.path.join(root[len(output_dir)+1:], name))
    return temp

def shrink_rootfs(output_dir):
    excludes = get_exclude_list(output_dir)
    for x in excludes:
       os.system("rm -rf %s/%s" % (output_dir, x))

def group_add(image_dir, group_name):
    """ Check whether given group exists in the chroot, if not add with regarding options."""

    import grp
    if os.fork() == 0:
        try:
            os.chroot(image_dir)

            if (grp.getgrnam(group_name)):
                pass
        except KeyError:
            run("/usr/sbin/groupadd %s" % group_name)
            if (group_name == "pulse"):
                run("/usr/sbin/useradd -d /var/run/pulse -g pulse pulse")
                run("/usr/bin/gpasswd -a pulse audio")
        except OSError:
            pass
        sys.exit(0)

def create_ptsp_rootfs(output_dir, repository, add_pkgs):
    try:
        # Add repository of the packages
        run('pisi --yes-all --destdir="%s" add-repo pardus %s' % (output_dir, repository))

        # Install default components, considiring exclusions
        install_cmd = "pisi --yes-all --ignore-comar --ignore-file-conflicts -D'%s' install" % output_dir
        for component in COMPONENTS:
            install_cmd += " -c %s" % component
        for pattern in PACKAGE_EXCLUDES:
            install_cmd += " -x %s" % pattern
        run(install_cmd)

        # Install default packages
        for package in PACKAGES.split():
            run('pisi --yes-all --ignore-comar --ignore-file-conflicts -D"%s" it %s' % (output_dir, package))

        # Install additional packages
        for package in add_pkgs:
            run('pisi --yes-all --ignore-comar --ignore-file-conflicts -D"%s" it %s' % (output_dir, package))

        # Create /etc from baselayout
        path = "%s/usr/share/baselayout/" % output_dir
        path2 = "%s/etc" % output_dir
        for name in os.listdir(path):
            run('cp -p "%s" "%s"' % (os.path.join(path, name), os.path.join(path2, name)))

        # Create character device
        os.mknod("%s/dev/null" % output_dir, 0666 | stat.S_IFCHR, os.makedev(1, 3))
        os.mknod("%s/dev/console" % output_dir, 0666 | stat.S_IFCHR, os.makedev(5, 1))

        # Create urandom character device
        os.mknod("%s/dev/urandom" % output_dir, 0666 | stat.S_IFCHR, os.makedev(1, 9))

        # Use proc and sys of the current system
        run('/bin/mount --bind /proc %s/proc' % output_dir)
        run('/bin/mount --bind /sys %s/sys' % output_dir)

        # run command in chroot
        def chrun(cmd):
            run('chroot "%s" %s' % (output_dir, cmd))

        chrun("/sbin/ldconfig")
        chrun("/sbin/update-environment")
        chroot_comar(output_dir)

        chrun("/bin/service dbus start")
        chrun("/usr/bin/pisi cp  baselayout")
        chrun("/usr/bin/pisi cp")
        chroot_call(output_dir, set_root_password)

        # If not existing, we must create 'pulse' user to run pulseaudio as system wide daemon
        group_add(output_dir, "pulse")

        # Create fuse group to get rid of localdev problems when using ldap users.
        # Also added a new udev rule (65-fuse.rules) to ptsp-client package to update /dev/fuse permissions regarding this change.
        group_add(output_dir, "fuse")

        chroot_call(output_dir, make_initramfs)
        # Create symbolic link
        kernel_image = glob.glob1("%s/boot" % output_dir, "kernel-*")[0]
        initramfs = glob.glob1("%s/boot" % output_dir, "initramfs-*")[0]
        run("ln -s %s %s/boot/latestkernel" % (kernel_image, output_dir))
        run("ln -s %s %s/boot/latestinitramfs" % (initramfs, output_dir))
        suffix = kernel_image.split("-", 1)[1]
        chrun("/sbin/depmod -a %s" % suffix)

        # Now it is Corporate2 release
        file(os.path.join(output_dir, "etc/pardus-release"), "w").write("Pardus Corporate 2\n")

        shrink_rootfs(output_dir)

        # Devices will be created in postinstall of ptsp-server
        os.unlink("%s/dev/console" % output_dir)
        os.unlink("%s/dev/null" % output_dir)
        os.unlink("%s/dev/urandom" % output_dir)

        shutil.rmtree("%s/lib/udev/devices" % output_dir)

        run('umount %s/proc' % output_dir)
        run('umount %s/sys' % output_dir)
    except KeyboardInterrupt:
        run('umount %s/proc' % output_dir, ignore_error=True)
        run('umount %s/sys' % output_dir, ignore_error=True)
        sys.exit(1)

def usage():
    print "\nUsage: build-client.py [option ...]\n"
    print "Following options are available:\n"
    print "    -h, --help            display this help and exit"
    print "    -o, --output          create the ptsp client rootfs into the given output path"
    print "    -r, --repository      ptsp client rootfs packages will be installed from this repository"
    print "    -a, --additional      install the given additional packages to ptsp client rootfs"

if __name__ == "__main__":

    try:
        opts, args = getopt.getopt(sys.argv[1:], "ho:r:a:",
                                    [ "help", "output=", "repository=", "--additional"])
    except getopt.GetoptError:
        usage()
        sys.exit(2)

    repository = "http://paketler.pardus.org.tr/corporate2/pisi-index.xml.xz"
    output_dir = None
    add_pkgs   = []

    for opt in opts:
        if opt[0] in ( "-h", "--help" ):
            usage()
            sys.exit(0)

        if opt[0] in ( "-o", "--output" ):
            output_dir = opt[1]

        if opt[0] in ( "-r", "--repository" ):
            repository = opt[1]

        if opt[0] in ( "-a", "--additional" ):
            add_pkgs.append(opt[1])

    if not output_dir:
        usage()
        print "\nPtsp client rootfs output directory must be specified..."
        sys.exit(1)

    create_ptsp_rootfs(output_dir, repository, add_pkgs)
