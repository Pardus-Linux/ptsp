#!/bin/bash
#
# Copyright 2006 - Jim McQuillan   <jam@Ltsp.org>
#              and Scott Balneaves <sbalneav@ltsp.org>
#
# This script is called by lbussd.
#
# It is called when devices appear or are removed.
#
# On a "add" new device event, it needs to do 3 things:
#
#    1)  Create a mountpoint in ~/Drives.
#    2)  Create a .desktop file in ~/Desktop
#    3)  Do the FUSE mount to associate the new folder with a device
#        on the thin client.
#
#
# On a "remove" device event does the following:
#
#    1)  Unmount the FUSE filesystem
#    2)  Remove the directory in ~/Drives
#    3)  Remove the .desktop file from the ~/Desktop directory so
#        the Icon goes away
#

ACTION=$1

DRIVEDIR=${DRIVE_DIR:-Drives}
ICON="usbpendrive_mount"

function create_icon {
  SHARENAME=$2
  cat <<-EOF  >${HOME}/Desktop/${SHARENAME}.desktop
	[Desktop Entry]
	Encoding=UTF-8
	Name=${1}
	GenericName=${1}
	URL=${3}
	Icon=${ICON}
	Type=Link
	# created by lbus_event_handler.sh
EOF
}

function remove_all {
  for mount in `grep ltspfs /proc/mounts | grep "${HOME}" | awk '{print $2}'`
  do
    fusermount -u -z ${mount}
  done

  if [ -n "${HOME}" ]; then

    if [ -d ${HOME}/${DRIVEDIR} ]; then
      for drive in ${HOME}/${DRIVEDIR}/*; do
        if [ -d ${drive} ]; then
          rmdir ${drive}
        fi
      done
    fi

    if [ -d ${HOME}/Desktop ]; then
      for desktop in ${HOME}/Desktop/*.desktop; do
        if [ -f ${desktop} ]; then
          if grep -q lbus_event_handler ${desktop}; then
            rm -f ${desktop}
          fi
        fi
      done
    fi

  fi
}

WS=${DISPLAY/:*/}

if [ ! -d ${HOME}/${DRIVEDIR} ]; then
  mkdir ${HOME}/${DRIVEDIR}
fi

case "${ACTION}" in
  add)
      DEVTYPE=$2
      SHARENAME=$3
      SIZE=$4
      DESC=$5
      case "${DEVTYPE}" in
          block)  mkdir "${HOME}/${DRIVEDIR}/${SHARENAME}"
                  /usr/bin/ltspfs ${WS}:/tmp/drives/${SHARENAME} \
                                   "${HOME}/${DRIVEDIR}/${SHARENAME}"
                  if [ -d ${HOME}/Desktop ]; then
                    create_icon "${DESC}" "${SHARENAME}" \
                                "${HOME}/${DRIVEDIR}/${SHARENAME}"
                  fi
                  ;;
       esac
       ;;

  remove)
      DEVTYPE=$2
      SHARENAME=$3
      SIZE=$4
      DESC=$5
      case "${DEVTYPE}" in
          block)  fusermount -u -z "${HOME}/${DRIVEDIR}/${SHARENAME}"
                  rmdir "${HOME}/${DRIVEDIR}/${SHARENAME}"
                  if [ -d ${HOME}/Desktop ]; then
                    rm -f "${HOME}/Desktop/${SHARENAME}.desktop"
                  fi
                  ;;
      esac
      ;;

  removeall)
      remove_all
      ;;

esac
