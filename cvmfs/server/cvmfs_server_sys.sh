#!/bin/sh
#
# This file is part of the CernVM File System
# This script takes care of creating, removing, and maintaining repositories
# on a Stratum 0/1 server

# This file should implement all platform specific functions. The other components of the
# cvmfs_server script should interact with the underlying system only through the functions
# defined here. This allows simple(r) unit testing of the "cvmfs_server" script

# Returns the major.minor.patch-build kernel version string
cvmfs_sys_uname() {
    uname -r | grep -oe '^[0-9]\+\.[0-9]\+.[0-9]\+-*[0-9]*'
}


cvmfs_sys_is_regular_file() {
    [ -f $1 ]
    echo $?
}


cvmfs_sys_file_is_executable() {
    [ -x $1 ]
    echo $?
}


cvmfs_sys_is_redhat() {
  cvmfs_sys_is_regular_file /etc/redhat-release
}


cvmfs_sys_get_fstype() {
  echo $(df -T /var/spool/cvmfs | tail -1 | awk {'print $2'})
}
