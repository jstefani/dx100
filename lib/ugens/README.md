# DX100Voice UGen

One 4-op FM voice in C++. `Engine_DX100` allocates these; chorus/phaser stay in sclang.

## norns

```
ssh we@norns.local
cd ~/dust/code/dx100/lib/ugens
chmod +x build_norns.sh
./build_norns.sh
```

First run clones SuperCollider source to `~/supercollider` for headers. Then **SYSTEM > RESTART**.

Plugin lands in `~/.local/share/SuperCollider/Extensions/dx100/` as
`DX100Voice.so` and `DX100Voice_scsynth.so`. Must be an **scsynth**
plugin (`server_type` 0). Compiling with `SUPERNOVA` defined makes
scsynth ignore it (`UGen 'DX100Voice' not installed`).

The sclang class is `lib/ugens/DX100Voice.sc` (compiled from dust).

## laptop

```
export SC_PATH=/path/to/supercollider/source
./build_norns.sh
```
