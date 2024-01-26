#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Test need 1 argument: path to device"
    exit 1
fi

$EXPECTED_DEVICE_SIZE=204800
MNT_DIR=`mktemp -d /tmp/test_mnt.XXXXX`

DEVICE_PATH=${1}

test_setup() {
  mkfs ${DEVICE_PATH}
  echo "mount ${DEVICE_PATH} to ${MNT_DIR}"
  mount ${DEVICE_PATH} ${MNT_DIR}
}

test_cleanup() {
  echo "umount ${MNT_DIR}/"
  umount ${MNT_DIR}/
  echo "rm -rf ${MNT_DIR}"
  rm -rf ${MNT_DIR}
}

test_module() {
  msg="my test message for this module"
  echo $msg > ${MNT_DIR}/test_file

  file_data=`cat ${MNT_DIR}/test_file`

  echo "Data read from the file: ${file_data}"
  echo "Data written to the file: ${msg}"

  if test "$file_data" == "$msg" ; then
      echo "SUCCESS"
  else
      echo "FAILED"
  fi
}

device_size=`blockdev --getsize ${DEVICE_PATH}`

if [ $EXPECTED_DEVICE_SIZE -ne device_size ]; then
  echo "Size of ${DEVICE_PATH} device is not EQUAL ${$EXPECTED_DEVICE_SIZE}"
  exit -1
fi

echo "Setup test environment"
test_setup
echo "Test environment is setup"

echo "Run test"
test_module

echo "Clean"
test_cleanup
echo "Environment is cleaned"