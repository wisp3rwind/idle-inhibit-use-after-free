Reproducer for https://github.com/swaywm/sway/issues/5680, https://github.com/swaywm/sway/issues/5325
```sh
git clone https://github.com/wisp3rwind/idle-inhibit-use-after-free.git
cd idle-inhibit-use-after-free
meson build
ninja -C build
./build/main
# Close the window
# -> use after free, ASAN abort
```
