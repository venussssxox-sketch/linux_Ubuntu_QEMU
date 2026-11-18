#!/bin/sh

MODULE_PATH="/root/kqueue_lab.ko"

pass_count=0
fail_count=0

module_is_loaded()
{
    grep -q '^kqueue_lab ' /proc/modules
}


unload_module()
{
    if module_is_loaded; then
        rmmod kqueue_lab
    fi
}

run_test()
{
    test_name="$1"
    test_program="$2"
    test_status=0

    printf '\n=== RUN: %s ===\n' "$test_name"

    if ! unload_module; then
        printf 'FAIL: could not remove old module\n'
        fail_count=$((fail_count + 1))
        return
    fi

    if ! insmod "$MODULE_PATH"; then
        printf 'FAIL: could not load %s\n' "$MODULE_PATH"
        fail_count=$((fail_count + 1))
        return
    fi

    if ! "$test_program"; then
        test_status=1
    fi

    if ! rmmod kqueue_lab; then
        printf 'FAIL: could not unload module after test\n'
        test_status=1
    fi

    if [ "$test_status" -eq 0 ]; then
        printf 'RESULT: PASS - %s\n' "$test_name"
        pass_count=$((pass_count + 1))
    else
        printf 'RESULT: FAIL - %s\n' "$test_name"
        fail_count=$((fail_count + 1))
    fi
}

run_lifecycle_test()
{
    holder_log="/tmp/kqueue-holder.log"
    holder_pid=0
    test_status=0

    printf '\n=== RUN: module lifecycle ===\n'

    if ! unload_module; then
        printf 'FAIL: could not remove old module\n'
        fail_count=$((fail_count + 1))
        return
    fi

    if ! insmod "$MODULE_PATH"; then
        printf 'FAIL: could not load module\n'
        fail_count=$((fail_count + 1))
        return
    fi

    /root/kqueue_hold_open_test > "$holder_log" 2>&1 &
    holder_pid=$!

    sleep 1

    if ! grep -q '^HOLDER_READY$' "$holder_log"; then
        printf 'FAIL: holder did not open the device\n'
        test_status=1
    fi

    if rmmod kqueue_lab 2>/tmp/kqueue-rmmod.log; then
        printf 'FAIL: module unloaded while device was open\n'
        test_status=1
    else
        printf 'PASS: unload refused while device was open\n'
    fi

    if ! wait "$holder_pid"; then
        printf 'FAIL: holder program returned failure\n'
        test_status=1
    fi

    if ! grep -q '^HOLDER_CLOSED$' "$holder_log"; then
        printf 'FAIL: holder did not close the device\n'
        test_status=1
    fi

    if module_is_loaded; then
        if rmmod kqueue_lab; then
            printf 'PASS: module unloaded after device close\n'
        else
            printf 'FAIL: module could not unload after close\n'
            test_status=1
        fi
    fi

    if [ "$test_status" -eq 0 ]; then
        printf 'RESULT: PASS - module lifecycle\n'
        pass_count=$((pass_count + 1))
    else
        printf 'RESULT: FAIL - module lifecycle\n'
        fail_count=$((fail_count + 1))
    fi
}
trap unload_module 0 1 2 15

run_test "basic read/write" "/root/kqueue_test"
run_test "ioctl state and errors" "/root/kqueue_ioctl_test"
run_test "blocking read" "/root/kqueue_blocking_test"
run_test "blocking write" "/root/kqueue_blocking_write_test"
run_test "signal interruption" "/root/kqueue_signal_interrupt_test"
run_test "poll readiness" "/root/kqueue_poll_test"
run_test "producer and consumer" "/root/kqueue_producer_consumer_test"
run_test "invalid user buffers" "/root/kqueue_error_test"
run_test "SIGIO notification" "/root/kqueue_sigio_test"

run_lifecycle_test

printf '\n=== REGRESSION SUMMARY ===\n'
printf 'passed: %d\n' "$pass_count"
printf 'failed: %d\n' "$fail_count"

if [ "$fail_count" -ne 0 ]; then
    exit 1
fi

exit 0
