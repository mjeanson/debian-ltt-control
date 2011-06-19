# Copyright (C) 2009 Benjamin Poirier
# Copyright (C) 2010 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>

# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

DEBUGFSROOT=$(awk '{if ($3 == "debugfs") print $2}' /proc/mounts | head -n 1)
MARKERSROOT=${DEBUGFSROOT}/ltt/markers
DEFAULTMODULES="ltt-trace-control ltt-marker-control ltt-tracer ltt-relay ltt-kprobes ltt-userspace-event ltt-statedump ipc-trace kernel-trace mm-trace net-trace fs-trace jbd2-trace syscall-trace trap-trace block-trace rcu-trace"

usage () {
	echo "Usage: $0 [OPTION]..." 1>&2
	echo "Connect lttng markers" 1>&2
	echo "" 1>&2
	echo "Options:" 1>&2
	printf "\t-l           Also activate locking markers (high traffic)\n" 1>&2
	printf "\t-n           Also activate detailed network markers (large size)\n" 1>&2
	printf "\t-i           Also activate input subsystem events (security implication: records keyboard inputs)\n" 1>&2
	echo "" 1>&2
	printf "\t-q           Quiet mode, suppress output\n" 1>&2
	printf "\t-h           Print this help\n" 1>&2
	echo "" 1>&2
}

if [ "$(id -u)" != "0" ]; then
	echo "Error: This script needs to be run as root." 1>&2
	exit 1;
fi

if [ ! "${DEBUGFSROOT}" ]; then
	echo "Error: debugfs not mounted" 1>&2
	exit 1;
fi

if [ ! -d "${MARKERSROOT}" ]; then
	#Try loading the kernel modules first
	for i in ${DEFAULTMODULES}; do
		modprobe $i
	done
	if [ ! -d "${MARKERSROOT}" ]; then
		echo "Error: LTT trace controller not found (did you compile and load LTTng?)" 1>&2
		exit 1;
	fi
fi

while getopts "lnqh" options; do
	case ${options} in
		l) LOCKING="0";;
		n) NETWORK="0";;
		q) QUIET="0";;
		i) INPUT="0";;
		h) usage;
			exit 0;;
		\?) usage;
			exit 1;;
	esac
done
shift $((${OPTIND} - 1))


if [ ! ${LOCKING} ]; then
	TESTS="${TESTS} -name lockdep -prune -o -name locking -prune -o"
else
	modprobe lockdep-trace
fi

if [ ! ${NETWORK} ]; then
	TESTS="${TESTS} -path '*/net/*_extended' -prune -o"
else
	modprobe net-extended-trace
fi

if [ ! ${INPUT} ]; then
	TESTS="${TESTS} -name input -prune -o"
fi

(eval "find '${MARKERSROOT}' ${TESTS} -name metadata -prune -o -name enable -print") | while read -r marker; do
	if [ ! ${QUIET} ]; then
		echo "Connecting ${marker%/enable}"
	fi
	echo 1 > ${marker}
done
