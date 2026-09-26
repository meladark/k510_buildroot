#!/bin/sh
#set -x
#echo "$MDEV $ACTION"

# Never fsck/remount the partition we are running from: fsck on a mounted,
# writable ext2 root corrupts it (files silently disappear after "mdev -s").
root_dev=$(sed -n 's#.*root=\(/dev/[^ ]*\).*#\1#p' /proc/cmdline)
[ "/dev/${MDEV}" = "${root_dev}" ] && exit 0


if [ "${MDEV:6:1}" =  "0" ];then
	if [ "${MDEV:8:1}" = "1" ]; then exit ;fi; #emmc part 1 can not mount;
	m_dir="/root/emmc/p${MDEV:8:1}"
	[ -d ${m_dir} ] || (mkdir -p "${m_dir}" )
elif [ "${MDEV:6:1}" =  "2" ];then
	m_dir="/root/sd/p${MDEV:8:1}"
	[ -d ${m_dir} ] || (mkdir -p "${m_dir}" )
fi


if [ "add" = $ACTION ];then		
	[ -d ${m_dir} ] || (mkdir -p "${m_dir}" )
	umount "${m_dir}"  2>/dev/null;
	
	if [ "${MDEV}" = "mmcblk0p3"  -o   "${MDEV}" = "mmcblk2p2" ]; then
		fsck.ext2 -p "/dev/${MDEV}" >/dev/null 2>&1;
		mount -t ext2 "/dev/${MDEV}" "${m_dir}";
	else
		fsck -p "/dev/${MDEV}" >/dev/null 2>&1;
		mount "/dev/${MDEV}" "${m_dir}";
	fi
	
else
	[ -d ${m_dir} ] && ( umount "${m_dir}" )
fi


