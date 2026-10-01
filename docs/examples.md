# Examples


## Shell Basics

Examples:

```sh
help
help wifi
wifi scan
wifi connect MySSID secret
wifi connect
ping example.com
sshcfg add pi2w pi@pi2w
sshcfg add lab pi@labhost -p 2222 -pw secret
ssh pi2w
lua
lua -e 'print(2+2)'
lua /root/lua/hello.lua
vi notes.txt
cat /root/init.sh
grep wifi /root/init.sh
find /sd -name "*.txt"
history
```

### Redirection And Pipes

Supported:

- `>`
- `>>`
- `<`
- single `|`

Examples:

```sh
echo hello > /root/test.txt
echo world >> /root/test.txt
cat < /root/test.txt
ls | head
```
Current limitations:

- no heredoc `<<`
- no multiple pipes like `a | b | c`
- no `||`

Supported chaining:

- `cmd1 && cmd2`
- `cmd1 ; cmd2`




## Filesystems

The shell exposes these roots:

- `/root` for internal LittleFS
- `/sd` for the SD card, when mounted

Examples:

```sh
ls /root
ls /sd
cd /sd
df
```



## Bluetooth Keyboard

Commands:

```sh
bt scan
bt scan -v
bt connect
bt status
bt clear
```

Behavior:

- `bt scan` scans for Bluetooth keyboards and connects to the first one found
- `bt connect` reconnects to the previously saved keyboard, scanning until it
  appears
- the saved keyboard address and the bond keys are stored in NVS, so they
  survive reflashing the app; `bt clear` erases them
- starting Bluetooth takes ~55 KB of RAM and stays loaded until reboot; it
  refuses to start, with a message, when less than 60 KB is free

## Wi-Fi Credential Storage

Saved Wi-Fi credentials are loaded in this order:

1. `/sd/.wifi_credentials`
2. `/root/.wifi_credentials`
3. NVS fallback

New successful `wifi connect <ssid> [password]` writes credentials to:

- SD if mounted
- otherwise internal flash

## SSH Host Storage

Saved SSH state currently uses internal storage:

- trusted host keys: `/root/.ssh/known_hosts`
- saved host aliases: `/root/.ssh/hosts`

Examples:

```sh
sshcfg add pi2w pi@pi2w
sshcfg add lab pi@labhost -p 2222 -pw secret
sshcfg list
sshcfg show pi2w
ssh pi2w
ssh pi2w uname -a
sshcfg rm pi2w
```

Notes:

- `sshcfg add` accepts either `<host>` plus `-l user`, or a single `user@host` target
- `-pw password` can be stored in the saved profile and reused by `ssh <alias>`
- saved passwords are stored in plain text in `/root/.ssh/hosts`
