# VoxelTerrain3D Hardware & Multi-Setting Benchmark Report

- **Status**: Completed Full Suite
- **Started (Local / UTC)**: `2026-10-21 17:52:58` / `2026-10-21 22:52:58`
- **Completed (Local / UTC)**: `2026-10-21 17:54:07` / `2026-10-21 22:54:07` (Total Duration: `69.28 s`)
- **Pre-Pass Terrain Reset**: `Enabled (undo_all (7 edits reverted))` — pristine terrain & statues restored before every pass
- **CPU**: AMD Ryzen 7 5800X 8-Core Processor (`x86_64`, 16 logical threads, 6 voxel worker threads)
- **GPU**: NVIDIA GeForce RTX 3080 — NVIDIA (Discrete GPU, API `1.4.351`, driver `vulkan` / `forward_plus`)
- **System RAM**: 32671 MiB physical total (11827 MiB physical free | 85919 MiB OS commit available)
- **OS & Engine**: Windows Windows (10.0.26200) | Godot `4.7.2-stable (official)`
- **Viewport & Voxel Config**: `1152x648` | `voxel_scale = 0.50 m` | `world_height_chunks = 8`

## Summary Across Settings Presets

| Preset | Completed (Local) | View / Prefetch | Upload Budget | Avg FPS | 1% Low FPS | Avg / p95 / p99 / Max (ms) | Flight FPS | CSG FPS | Settle (ms) | Commits/s | Avg Blast (ms) | Nav Sync / Query (ms) | Peak VRAM (MiB) | Peak Draw Calls |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Low / Fast Stream** | `2026-10-21 17:53:13` | `6 / 1` | `1.5 ms` | **387.8** | 153.6 | `2.58 / 4.48 / 6.51 / 66.23` | 477.2 | 351.0 | 3501 | 641.0 | 46.14 | `214.2 / 0.106` | 385.4 | 1023 |
| **Balanced Default** | `2026-10-21 17:53:25` | `8 / 2` | `2.5 ms` | **288.0** | 50.6 | `3.47 / 5.20 / 19.76 / 92.70` | 305.6 | 269.9 | 962 | 458.8 | 28.64 | `47.9 / 0.181` | 406.4 | 1203 |
| **High Throughput Upload** | `2026-10-21 17:53:38` | `10 / 3` | `5.0 ms` | **265.2** | 49.9 | `3.77 / 5.35 / 20.02 / 138.66` | 269.5 | 254.0 | 1163 | 553.6 | 38.38 | `51.1 / 0.224` | 406.8 | 1326 |
| **Extended Horizon** | `2026-10-21 17:53:53` | `14 / 4` | `6.0 ms` | **206.2** | 31.3 | `4.85 / 7.10 / 31.95 / 136.00` | 177.9 | 220.8 | 3017 | 793.3 | 25.69 | `3006.5 / 0.136` | 470.8 | 1617 |
| **Full Collision Stress** | `2026-10-21 17:54:06` | `12 / 2` | `8.0 ms` | **174.6** | 14.0 | `5.73 / 12.14 / 71.57 / 140.59` | 147.3 | 158.9 | 631 | 535.7 | 31.35 | `140.1 / 0.158` | 471.5 | 1468 |
