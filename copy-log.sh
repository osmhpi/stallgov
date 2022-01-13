#!/bin/bash

usage() {
    echo "Usage: $0 [-s SOURCE (/sys/kernel/debug/memutil/log) | -i INTERVAL (8)] <output_dir> <core_number>"
    echo ""
    echo "The log of memutil is only large enough for a couple of seconds and is cleared after each read to have space for the next data."
    echo "This utility script periodically copies the log created by memutil and appends it to log.txt in the given output dir."
    echo "Also one separate log for each core is created."
    echo "Parameters:"
    echo -e "\t-s SOURCE: Specifies the path to the memutil log"
    echo -e "\t-i INTERVAL: Specifies the interval with which the log should be copied"
    echo -e "\t-c: Clear the existing log files in the given output dir."
    echo -e "\t<output_dir>: Directory where the copied log and separate log files should be stored. Has to exist for this command to work"
    echo -e "\t<core_number>: The amount of cores memutil runs on. This is needed to know how many separate log files have to be created."
}

copy_log_part() {
    cat $1 | grep "^$2," >> $3/log-$2.txt
}

clear_logs() {
    echo "Clearing logs..."
    rm -f $1/log.txt
    for i in $(seq 0 1 $(($2-1)))
    do
        rm -f $1/log-$i.txt
    done
}

[ $# -eq 0 ] && usage && exit 1

SOURCE_FILE=/sys/kernel/debug/memutil/log
INTERVAL=8
SHOULD_CLEAR=false

while getopts ":hs:i:c" arg
do
    case $arg in
        s) # Specify source
            SOURCE_FILE=${OPTARG}
            ;;
        i) # Specify interval
            INTERVAL=${OPTARG}
            ;;
        c) # Clear the existing logs
            SHOULD_CLEAR=true
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

if [ ! -d "$OUTPUT_DIR" ]
then
    echo "Given output directory does not exist!"
    usage
    exit 1
fi

if [ $SHOULD_CLEAR ]
then
    clear_logs $OUTPUT_DIR $CORE_NUMBER
fi

step_log() {
    echo -n "."
    if [ ! -e "$SOURCE_FILE" ]
    then
        echo "Source file missing"
        return
    fi

    cp $SOURCE_FILE $OUTPUT_DIR/log-current.txt
    cat $OUTPUT_DIR/log-current.txt >> $OUTPUT_DIR/log.txt
    for i in $(seq 0 1 $((CORE_NUMBER-1)))
    do
        copy_log_part $OUTPUT_DIR/log-current.txt $i $OUTPUT_DIR
    done
    rm $OUTPUT_DIR/log-current.txt
}

exit_script() {
    step_log
    echo -n "Copy-log terminated"
    exit
}

trap exit_script SIGINT SIGTERM

echo "Running"
while true
do
    sleep $INTERVAL
    step_log
done

exit 0
