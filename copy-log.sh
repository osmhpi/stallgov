#!/bin/bash

usage() {
    echo "Usage: $0 [-s SOURCE (/sys/kernel/debug/memutil/log) | -i INTERVAL (60)] <output_dir> <core_number>"
}

copy_log_part() {
    cat $1 | grep "^$2," >> $3/log-$2.txt
}

[ $# -eq 0 ] && usage && exit 1

SOURCE_FILE=/sys/kernel/debug/memutil/log
INTERVAL=60

while getopts ":hs:i:" arg
do
    case $arg in
        s) # Specify source
            SOURCE_FILE=${OPTARG}
            ;;
        i) # Specify interval
            INTERVAL=${OPTARG}
            ;;
        h | *) # Display help
            usage
            exit 0
            ;;
    esac
done

shift $((OPTIND-1))
if [[ $# -lt 2 ]]
then
    usage
    exit 1
fi

OUTPUT_DIR=$1
CORE_NUMBER=$2

echo "Running"
while true
do
    sleep $INTERVAL
    echo -n "."
    if [ ! -e "$SOURCE_FILE" ]
    then
        echo "Source file missing"
        continue
    fi
    
    cp $SOURCE_FILE $OUTPUT_DIR/log-current.txt
    cat $OUTPUT_DIR/log-current.txt >> $OUTPUT_DIR/log.txt
    for i in $(seq 0 1 $((CORE_NUMBER-1)))
    do
        copy_log_part $OUTPUT_DIR/log-current.txt $i $OUTPUT_DIR
    done
done

exit 0
