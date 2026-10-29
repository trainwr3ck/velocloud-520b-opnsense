#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# The one write path to the drive. Source it, call it, never restate it.
#
#   fs_window_open [path]   drop the union over <path> (default /boot), then
#                           mount -uw /
#   fs_window_close         mount -ur / - the mount state is left as found
#
# The mount state is read from `mount`, never assumed, and left exactly as found.
# A write that lands in a unionfs layer is RAM and is gone at reboot, so the
# union comes off first. Never `umount -f` a live filesystem. This window does
# not reach /conf on a live box - stage config into the unmounted image on the
# build host instead.
#
# The caller must already define log(), warn() and die().

FS_WAS_RO=0
FS_WINDOW=0

fs_window_open() {
	path=${1:-/boot}
	if mount | grep " on $path (" | grep -q unionfs; then
		umount "$path" ||
			die "umount $path failed - the union over it is busy. Never umount -f
a live filesystem: stop whatever holds it first."
		log "dropped the unionfs over $path"
	fi
	if mount | grep ' / ' | grep -q read-only; then FS_WAS_RO=1; fi
	if [ "$FS_WAS_RO" = 1 ]; then
		mount -uw / || die "mount -uw / failed - nothing written"
		FS_WINDOW=1
		log "root window open"
	fi
	# Files are only ever swapped by atomic rename on the same filesystem, so a
	# half-write is not possible even if the caller dies from here on.
	trap 'fs_window_close' EXIT HUP INT TERM
}

fs_window_close() {
	[ "$FS_WINDOW" = 1 ] || return 0
	mount -ur / 2>/dev/null || {
		warn "mount -ur / failed - remount it yourself, / must not stay rw"
		return 1
	}
	FS_WINDOW=0
	log "root remounted read-only"
}
