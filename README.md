Inspired By @ryanistr OPlus-Brightness-Adaptor-For-Transsion

Why? This one specially made for my device (TECNO LH8n) that still have brightness sudden dim. It may work on other IPS display

How to use? Add to vendor and add the fs context Like this: 

```
/vendor/bin/hw/yamada\.obright-V1@3\.0-service    u:object_r:yamada_obright_exec:s0
```

As for SELinux.cil (vendor/etc/selinux/vendor_selinux.cil)

```
# Define the domain and the executable type
type yamada_obright, domain;
type yamada_obright_exec, exec_type, vendor_file_type, file_type;

# Tell init that this is a daemon that transitions to the yamada_obright domain
init_daemon_domain(yamada_obright)

# Allow the service to read system properties (to catch debug.tracing.screen_brightness)
# Note: Depending on OPlus's specific sepolicy, you might need to change 'default_prop' to 'debug_prop' or whatever specific context that property holds.
get_prop(yamada_obright, default_prop)

# Allow the service to search the sysfs directory and write to the brightness node
allow yamada_obright sysfs_leds:dir search;
allow yamada_obright sysfs_leds:file rw_file_perms;

# Allow the service to write to logcat (since we linked -llog in the C code)
allow yamada_obright logdw_socket:sock_file write;
allow yamada_obright logd:unix_stream_socket connectto;
```

Support Me: <br />
https://sociabuzz.com/kanagawa_yamada/tribe (Global) <br />
https://t.me/KLAGen2/86 (QRIS) <br />
