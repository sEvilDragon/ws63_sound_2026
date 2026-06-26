import sys

filepath = r"D:/MCU/hispark/project/2026_sound/fbb_ws63/src/main/receiving end/includes/dlan/dlan.cpp"

with open(filepath, 'rb') as f:
    content = f.read()

changes = 0

# 1. Replace select timeout
old = b'int ret = lwip_select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr);'
new = b'timeval tv = {1, 0};\n        int ret = lwip_select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);'
if old in content:
    content = content.replace(old, new, 1)
    changes += 1
    print("OK: select timeout")
else:
    print("MISS: select timeout")

# 2. Add stop check in select loop
old = b'if (ret < 0) {'
# Find the one inside the select loop (after the select call)
idx = content.find(b'lwip_select')
if idx >= 0:
    idx2 = content.find(b'if (ret < 0) {', idx)
    if idx2 >= 0:
        content = content[:idx2] + b'if (s_stop_requested) break;\n        ' + content[idx2:]
        changes += 1
        print("OK: stop check in select loop")
    else:
        print("MISS: ret < 0 after select")
else:
    print("MISS: lwip_select not found")

# 3. Replace while(true) before select with while(!s_stop_requested)
old = b'while (true) {\n        memset(&read_fds, 0, sizeof(read_fds));\n        FD_SET(ssdp_sock'
new = b'while (!s_stop_requested) {\n        memset(&read_fds, 0, sizeof(read_fds));\n        FD_SET(ssdp_sock'
if old in content:
    content = content.replace(old, new, 1)
    changes += 1
    print("OK: while loop condition")
else:
    print("MISS: while loop")

# 4. Add stop check to is_ready wait loop
old = b'while (!is_ready) {'
new = b'while (!is_ready && !s_stop_requested) {'
if old in content:
    content = content.replace(old, new, 1)
    changes += 1
    print("OK: is_ready loop")
else:
    print("MISS: is_ready loop")

# 5. Add stop check to IP wait loop
old = b'while (true) {\n        ssdp_ip_get();'
new = b'while (true) {\n        if (s_stop_requested) return;\n        ssdp_ip_get();'
if old in content:
    content = content.replace(old, new, 1)
    changes += 1
    print("OK: IP wait loop")
else:
    print("MISS: IP wait loop")

# 6. Add cleanup after select loop exits
old = b'    }\n}\n\nbool dlan::ssdp_set()'
new = b'    }\n\n    dlan_stop();\n    osal_printk("[DLNA] exited\\r\\n");\n}\n\nbool dlan::ssdp_set()'
if old in content:
    content = content.replace(old, new, 1)
    changes += 1
    print("OK: cleanup after exit")
else:
    print("MISS: cleanup after exit")

# 7. Add request_stop/reset_stop after dlan_stop
old = b'    if (http_sock >= 0) {\n        lwip_close(http_sock);\n        http_sock = -1;\n    }\n}'
new = b'''    if (http_sock >= 0) {
        lwip_close(http_sock);
        http_sock = -1;
    }
}

void dlan::request_stop()
{
    s_stop_requested = true;
}

void dlan::reset_stop()
{
    s_stop_requested = false;
}'''
if old in content:
    content = content.replace(old, new, 1)
    changes += 1
    print("OK: request_stop/reset_stop")
else:
    print("MISS: dlan_stop end")

with open(filepath, 'wb') as f:
    f.write(content)

print(f"Done. {changes} changes applied.")
