lttctl(1) -- linux trace toolkit control
========================================

## SYNOPSIS

`lttctl` [<options>]... [<tracename>]

## DESCRIPTION

`lttctl` is a small program that controls LTTng through liblttctl.

## OPTIONS

  * `-h`, `--help`:
    Show summary of options.

  * `-c`, `--create`:
    Create a trace.

  * `-d`, `--destroy`:
    Destroy a trace.

  * `-s`, `--start`:
    Start a trace.

  * `-p`, `--pause`:
    Pause a trace.

  * `--transport TRANSPORT`:
    Set trace's transport. (ex. relay-locked or relay)

  * `-o`, `--option OPTION`
    Set options, following operations are supported:
        channel.<channelname>.enable=
        channel.<channelname>.overwrite=
        channel.<channelname>.bufnum=
        channel.<channelname>.bufsize= (in bytes, rounded to next power of 2)
        <channelname> can be set to all for all channels
        channel.<channelname>.switch_timer= (timer interval in ms)

  * `-C`, `--create_start`:
    Create and start a trace.

  * `-D`, `--pause_destroy`:
    Pause and destroy a trace.

  * `-w`, `--write PATH`:
    Path for write trace datas. For -c, -C, -d, -D options.

  * `-a`, `--append`:
    Append to trace, For -w option.

  * `-n`, `--dump_threads NUMBER`:
    Number of lttd threads, For -w option.

  * `--channel_root PATH`:
    Set channels root path, For -w option. (ex. /mnt/debugfs/ltt)

## AUTHOR

`lttctl` was written by Zhao Lei &lt;zhaolei@cn.fujitsu.com&gt;, Gui Jianfeng
&lt;guijianfeng@cn.fujitsu.com&gt;, and Mathieu Desnoyers
&lt;mathieu.desnoyers@efficios.com&gt;.

This manual page was written by Jon Bernard &lt;jbernard@debian.org&gt;, for
the Debian project (and may be used by others).
