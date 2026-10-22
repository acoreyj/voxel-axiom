extends SceneTree
## One-off cave/overhang scout. Run headless:
##   godot --headless --path . --script res://tools/scout_caves.gd
## Prints columns whose surface is significantly below the local neighborhood.

func _init() -> void:
	call_deferred("_run")

func _run() -> void:
	var packed: PackedScene = load("res://voxel_terrain_demo.tscn")
	var scene := packed.instantiate()
	root.add_child(scene)
	await process_frame
	await process_frame
	var terrain = scene.get_node("VoxelTerrain3D")
	var waited := 0.0
	while waited < 90.0:
		if terrain.core_idle() and terrain.fully_committed():
			break
		await process_frame
		waited += 1.0 / 60.0
	print("[Scout] world wait %.1fs idle=%s committed=%s" % [waited, terrain.core_idle(), terrain.fully_committed()])

	var samples: Array[Dictionary] = []
	for x in range(-160, 161, 4):
		for z in range(-160, 161, 4):
			var hit: Vector3 = terrain.raycast_terrain(Vector3(x, 127, z), Vector3(x, 0, z))
			if is_finite(hit.x):
				samples.append({"x": float(x), "z": float(z), "y": hit.y})
	print("[Scout] samples=", samples.size())

	var candidates: Array[Dictionary] = []
	for s in samples:
		var local_sum := 0.0
		var local_count := 0
		for o in samples:
			if absf(o["x"] - s["x"]) <= 12.0 and absf(o["z"] - s["z"]) <= 12.0:
				local_sum += float(o["y"])
				local_count += 1
		if local_count == 0:
			continue
		var avg := local_sum / local_count
		var depth: float = avg - float(s["y"])
		if depth > 6.0:
			candidates.append({"x": s["x"], "z": s["z"], "y": s["y"], "depth": depth})
	candidates.sort_custom(func(a, b): return a["depth"] > b["depth"])
	print("[Scout] candidates=", candidates.size())
	for i in range(mini(30, candidates.size())):
		var c = candidates[i]
		print("[Scout] %02d x=%.0f z=%.0f y=%.1f depth=%.1f" % [i, c["x"], c["z"], c["y"], c["depth"]])

	quit()
