# Shader Reference

Last generated: 2026-07-21

Coverage summary: `193` documented functions, `13` undocumented functions, `69` documented uniform fields, `120` undocumented uniform fields.

Undocumented entries are omitted by default. Use `--include-undocumented` to list them.

## brush_accumulate.glsl

`modules/gaussian_splatting/shaders/brush_accumulate.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>fetch_color(vec2 uv)</code></pre></td>
      <td>Fetch the source color used by the brush accumulation pass.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Accumulate brush contributions into the offscreen resolve target.</td>
    </tr>
  </tbody>
</table>


## gaussian_splat.frag.glsl

`modules/gaussian_splatting/shaders/gaussian_splat.frag.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Fragment entry point that resolves the final Gaussian splat color.</td>
    </tr>
  </tbody>
</table>


## gaussian_splat.glsl

`modules/gaussian_splatting/shaders/gaussian_splat.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Vertex-stage entry point that prepares splat varyings.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Fragment-stage entry point that shades the current splat.</td>
    </tr>
  </tbody>
</table>


## gaussian_splat.vert.glsl

`modules/gaussian_splatting/shaders/gaussian_splat.vert.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Vertex entry point for Gaussian splat quad expansion.</td>
    </tr>
  </tbody>
</table>


## gs_shadow_blit.glsl

`modules/gaussian_splatting/shaders/gs_shadow_blit.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Vertex entry point for the shadow-map blit quad.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Fragment entry point that copies the shadow-map sample to the output.</td>
    </tr>
  </tbody>
</table>

### Uniform Blocks

#### Params (params)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>uv_scale_offset</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>xy = scale, zw = offset</td>
    </tr>
    <tr>
      <td><pre><code>invert_depth</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>1.0 to invert (reversed depth)</td>
    </tr>
  </tbody>
</table>

#### Params (params)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>uv_scale_offset</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>xy = scale, zw = offset</td>
    </tr>
    <tr>
      <td><pre><code>invert_depth</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>1.0 to invert (reversed depth)</td>
    </tr>
  </tbody>
</table>


## color_grading_binning.glsl

`modules/gaussian_splatting/shaders/includes/color_grading_binning.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rgb_to_hsv(vec3 c)</code></pre></td>
      <td>Binning Stage Color Grading Applied after SH evaluation, before packing into ProjectedGaussian Cost: ~20 ALU operations per splat = ~0.02ms for 1M splats RGB to HSV (copy from tonemap.glsl)</td>
    </tr>
    <tr>
      <td><pre><code>hsv_to_rgb(vec3 c)</code></pre></td>
      <td>HSV to RGB (copy from tonemap.glsl)</td>
    </tr>
    <tr>
      <td><pre><code>apply_color_grading_binning(vec3 color, uint instance_id)</code></pre></td>
      <td>Apply color grading to splat color (binning stage) Operates in linear color space, before R11G11B10 packing. The grading parameters are fetched per-instance from instance_grading_buffer, which the binning shader binds alongside instance_buffer at binding 20.</td>
    </tr>
  </tbody>
</table>


## gaussian_splat_common_inc.glsl

`modules/gaussian_splatting/shaders/includes/gaussian_splat_common_inc.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>quaternion_to_matrix(vec4 q)</code></pre></td>
      <td>Convert a quaternion to a rotation matrix.</td>
    </tr>
    <tr>
      <td><pre><code>build_covariance(vec3 scale, vec4 rotation)</code></pre></td>
      <td>Build a 3D covariance matrix from scale and rotation.</td>
    </tr>
    <tr>
      <td><pre><code>compute_eigen(mat2 cov)</code></pre></td>
      <td>Compute the 2D covariance eigensystem for ellipse reconstruction.</td>
    </tr>
    <tr>
      <td><pre><code>get_sigma_multiplier()</code></pre></td>
      <td>Return the scene sigma multiplier override or its default.</td>
    </tr>
    <tr>
      <td><pre><code>get_gaussian_count()</code></pre></td>
      <td>Return the encoded Gaussian count stored in scene metadata.</td>
    </tr>
    <tr>
      <td><pre><code>compute_projected_covariance(vec3 view_pos, vec3 scale, vec4 rotation, vec2 viewport_size)</code></pre></td>
      <td>Project 3D covariance into screen space for the current viewport.</td>
    </tr>
    <tr>
      <td><pre><code>covariance_to_conic(mat2 cov2d)</code></pre></td>
      <td>Convert a 2D covariance matrix into conic coefficients.</td>
    </tr>
    <tr>
      <td><pre><code>gaussian_get_first_order_count(uint meta)</code></pre></td>
      <td>Extract the first-order SH coefficient count from packed metadata.</td>
    </tr>
    <tr>
      <td><pre><code>gaussian_get_high_order_count(uint meta)</code></pre></td>
      <td>Extract the higher-order SH coefficient count from packed metadata.</td>
    </tr>
    <tr>
      <td><pre><code>gaussian_get_encoded_count(uint meta)</code></pre></td>
      <td>Extract the total encoded SH coefficient count from packed metadata.</td>
    </tr>
    <tr>
      <td><pre><code>gaussian_get_sh_encoding(uint meta)</code></pre></td>
      <td>Extract the SH encoding mode from packed metadata.</td>
    </tr>
    <tr>
      <td><pre><code>decode_rgb9e5(uint packed)</code></pre></td>
      <td>Decode packed RGB9E5 color data into linear RGB.</td>
    </tr>
    <tr>
      <td><pre><code>dither_noise(vec2 frag_coord)</code></pre></td>
      <td>Simple hash-based noise for dithering (spatially varying) Uses fragment position to generate pseudo-random value in [-0.5, 0.5]</td>
    </tr>
    <tr>
      <td><pre><code>dither_noise_rgb(vec2 frag_coord)</code></pre></td>
      <td>Generate RGB dither noise using different offsets for each channel This breaks up color banding from RGB9E5 quantization</td>
    </tr>
    <tr>
      <td><pre><code>compute_sh_basis_with_bands(vec3 dir, uint max_band, out float basis_values[16])</code></pre></td>
      <td>SH sign convention (ISSUE-038): Condon-Shortley phase included. This evaluation uses the real spherical harmonics basis with the Condon-Shortley (CS) phase factor applied.  Odd-m basis functions carry a leading minus sign (e.g. Y_1^{-1} = -C1*y, Y_1^1 = -C1*x). PLY coefficients from 3DGS training (Kerbl et al. 2023) are stored with CS phase already baked in, so they are consumed here without any sign adjustment.  The import side (ply_loader.cpp, assemble_sh_coefficients) documents the same convention. Compute SH basis functions up to the specified band level basis_values[0] = DC (l=0) basis_values[1-3] = 1st order (l=1) basis_values[4-8] = 2nd order (l=2) basis_values[9-15] = 3rd order (l=3)</td>
    </tr>
    <tr>
      <td><pre><code>compute_sh_basis(vec3 dir, out float basis_values[16])</code></pre></td>
      <td>Legacy compute_sh_basis for backwards compatibility (computes all bands)</td>
    </tr>
    <tr>
      <td><pre><code>evaluate_sh_color_with_bands(uint gaussian_index, vec3 dir, uint sh_band_level)</code></pre></td>
      <td>Evaluate SH color with configurable band level sh_band_level: 0=DC only, 1=1st order, 2=2nd order, 3=3rd order</td>
    </tr>
    <tr>
      <td><pre><code>evaluate_sh_color(uint gaussian_index, vec3 dir)</code></pre></td>
      <td>Legacy evaluate_sh_color for backwards compatibility (uses all available bands)</td>
    </tr>
    <tr>
      <td><pre><code>evaluate_sh_color_dithered(uint gaussian_index, vec3 dir, uint sh_band_level, vec2 frag_coord)</code></pre></td>
      <td>Evaluate SH color with dithering to mitigate RGB9E5 quantization banding frag_coord: fragment screen position (gl_FragCoord.xy) for spatially-varying dither sh_band_level: 0=DC only, 1=1st order, 2=2nd order, 3=3rd order</td>
    </tr>
  </tbody>
</table>

### Uniform Blocks

#### SceneData (scene_data)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>camera_position</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>xyz: world camera position, w: time or unused</td>
    </tr>
    <tr>
      <td><pre><code>viewport</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: width, y: height, z: near plane, w: far plane</td>
    </tr>
    <tr>
      <td><pre><code>misc</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: sigma multiplier override, y: gaussian count, z,w: unused</td>
    </tr>
  </tbody>
</table>


## gs_culling_utils.glsl

`modules/gaussian_splatting/shaders/includes/gs_culling_utils.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gs_hash_u32(uint v)</code></pre></td>
      <td>Hash a 32-bit value for deterministic culling randomness.</td>
    </tr>
    <tr>
      <td><pre><code>gs_should_distance_cull(uint p_stable_splat_key, float world_distance)</code></pre></td>
      <td>Returns true if splat should be culled based on distance. p_stable_splat_key must be stable across camera-motion-induced sort order changes.</td>
    </tr>
    <tr>
      <td><pre><code>gs_keep_overlap_record(uint gaussian_idx, uint instance_id, uint tile_idx)</code></pre></td>
      <td>Decide whether to keep an overlap record for diagnostics or coverage sampling.</td>
    </tr>
  </tbody>
</table>


## gs_deformation.glsl

`modules/gaussian_splatting/shaders/includes/gs_deformation.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gs_wind_hash_u32(uint v)</code></pre></td>
      <td>Hash an instance identifier for wind phase variation.</td>
    </tr>
    <tr>
      <td><pre><code>gs_decode_instance_wind_mode(float encoded_mode)</code></pre></td>
      <td>Decode the per-instance wind mode from the packed float value.</td>
    </tr>
    <tr>
      <td><pre><code>gs_is_wind_enabled_for_instance(float encoded_mode, vec4 wind_time_config)</code></pre></td>
      <td>Check whether wind deformation is enabled for the current instance.</td>
    </tr>
  </tbody>
</table>


## gs_directional_shadow.glsl

`modules/gaussian_splatting/shaders/includes/gs_directional_shadow.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gs_directional_shadow(uint idx, vec3 vertex, vec3 geo_normal, float taa_frame_count, float receiver_bias)</code></pre></td>
      <td>Directional shadow sampling for Gaussian splats. Adapted from Godot's forward clustered directional shadow path (no soft shadows).</td>
    </tr>
    <tr>
      <td><pre><code>gs_omni_shadow_factor(uint idx, vec3 vertex, vec3 geo_normal, float taa_frame_count, out float attenuation)</code></pre></td>
      <td>Omni shadow factor + attenuation (hard shadow path, soft shadows disabled).</td>
    </tr>
    <tr>
      <td><pre><code>gs_spot_shadow_factor(uint idx, vec3 vertex, vec3 geo_normal, float taa_frame_count, out float attenuation)</code></pre></td>
      <td>Spot shadow factor + attenuation (hard shadow path, soft shadows disabled).</td>
    </tr>
  </tbody>
</table>


## gs_eigen_binning.glsl

`modules/gaussian_splatting/shaders/includes/gs_eigen_binning.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>compute_opacity_aware_sigma(float opacity, float visibility_threshold, float max_sigma)</code></pre></td>
      <td>Opacity-Aware Bounding (FlashGS Optimization) Computes the effective sigma multiplier based on opacity and visibility threshold.  Reduces tile-Gaussian pairs by ~94% for low-opacity splats. Reference: FlashGS — Efficient Gaussian Splatting with Adaptive Bounds Compute the sigma multiplier for opacity-aware bounds. Returns the effective number of sigmas to use based on opacity. For high opacity (close to 1.0), returns close to max_sigma (conservative). For low opacity, returns a smaller value (aggressive culling).</td>
    </tr>
    <tr>
      <td><pre><code>compute_eigen(mat2 cov)</code></pre></td>
      <td>Compute eigenvalues and eigenvectors for tile binning heuristics.</td>
    </tr>
  </tbody>
</table>


## gs_lighting_bridge.glsl

`modules/gaussian_splatting/shaders/includes/gs_lighting_bridge.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>sc_use_light_projector()</code></pre></td>
      <td>Compute-shader fallback: projector lights are unavailable.</td>
    </tr>
    <tr>
      <td><pre><code>sc_use_light_soft_shadows()</code></pre></td>
      <td>Compute-shader fallback: soft shadows are unavailable.</td>
    </tr>
    <tr>
      <td><pre><code>sc_projector_use_mipmaps()</code></pre></td>
      <td>Compute-shader fallback: projector mipmaps are unavailable.</td>
    </tr>
    <tr>
      <td><pre><code>sc_soft_shadow_samples()</code></pre></td>
      <td>Match Godot's SHADOW_QUALITY_SOFT_LOW default (4 taps) so gaussian splat shadows have comparable quality to mesh shadows. Without PCF, each pixel gets a binary 0/1 shadow from a single compare, producing harsh per-pixel outlines.</td>
    </tr>
    <tr>
      <td><pre><code>sc_luminance_multiplier()</code></pre></td>
      <td>Compute-shader fallback: keep luminance neutral in the bridge path.</td>
    </tr>
  </tbody>
</table>


## gs_lighting_common.glsl

`modules/gaussian_splatting/shaders/includes/gs_lighting_common.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>cluster_get_item_range(uint offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to)</code></pre></td>
      <td>Read the packed item range for one clustered-light record.</td>
    </tr>
    <tr>
      <td><pre><code>cluster_get_range_clip_mask(uint i, uint z_min, uint z_max)</code></pre></td>
      <td>Compute the clip mask used to reject clustered-light slices outside range.</td>
    </tr>
    <tr>
      <td><pre><code>gs_use_clustered_lights()</code></pre></td>
      <td>Return whether clustered lights are active for this frame.</td>
    </tr>
    <tr>
      <td><pre><code>gs_compute_receiver_bias(float representative_radius)</code></pre></td>
      <td>Computes the canonical receiver bias for shadow sampling. Per the contract documented in gs_render_params.glsl:77-80 and gaussian_gpu_layout.h:456-459: shadow_bias_config.x is the scale-by-radius multiplier, .y is the minimum bias, .z is the optional maximum clamp (>0.0 enables it). Pass representative_radius = max(g.scale.x, g.scale.y, g.scale.z) from the per-splat path. Pass 0.0 from paths without a radius source — the result collapses to the .y minimum, clamped by .z.</td>
    </tr>
  </tbody>
</table>


## gs_quat_utils.glsl

`modules/gaussian_splatting/shaders/includes/gs_quat_utils.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>quaternion_to_matrix(vec4 q)</code></pre></td>
      <td>Convert a quaternion to a rotation matrix.</td>
    </tr>
    <tr>
      <td><pre><code>gs_quat_rotate(vec4 q, vec3 v)</code></pre></td>
      <td>Rotate a vector by a quaternion.</td>
    </tr>
    <tr>
      <td><pre><code>gs_quat_mul(vec4 a, vec4 b)</code></pre></td>
      <td>Multiply two quaternions.</td>
    </tr>
  </tbody>
</table>


## gs_render_params.glsl

`modules/gaussian_splatting/shaders/includes/gs_render_params.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gs_get_sh_band_level()</code></pre></td>
      <td>Helper to get current SH band level from params</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_sh_amortization_divisor()</code></pre></td>
      <td>Return the SH amortization divisor from render params.</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_sh_amortization_phase()</code></pre></td>
      <td>Return the SH amortization phase from render params.</td>
    </tr>
    <tr>
      <td><pre><code>gs_is_sh_amortization_enabled()</code></pre></td>
      <td>Return whether SH amortization is enabled.</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_sh_amortization_force_update()</code></pre></td>
      <td>Return whether SH amortization should force an update this frame.</td>
    </tr>
    <tr>
      <td><pre><code>gs_is_opacity_aware_culling_enabled()</code></pre></td>
      <td>Helper to check if opacity-aware culling is enabled</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_visibility_threshold()</code></pre></td>
      <td>Helper to get visibility threshold (tau)</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_alpha_clip()</code></pre></td>
      <td>Optional per-splat hard cull threshold. Splats with opacity <= this value are dropped before any projection or binning work. 0.0 disables the cull. CPU-side is clamped to [0, 0.99].</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_lod_blend_factor()</code></pre></td>
      <td>Helper to get LOD blend factor from params (LODGE technique)</td>
    </tr>
    <tr>
      <td><pre><code>gs_is_lod_blend_enabled()</code></pre></td>
      <td>Helper to check if LOD blending is enabled</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_overlap_keep_ratio()</code></pre></td>
      <td>Return the overlap keep ratio used by culling and diagnostics.</td>
    </tr>
    <tr>
      <td><pre><code>gs_is_wind_enabled()</code></pre></td>
      <td>Return whether wind deformation is enabled.</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_sphere_effector_count()</code></pre></td>
      <td>Active sphere effector count for this pass.</td>
    </tr>
    <tr>
      <td><pre><code>gs_is_sphere_effector_enabled()</code></pre></td>
      <td>Return whether the sphere effector is enabled.</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_hotspot_pressure_threshold()</code></pre></td>
      <td>Hotspot-aware pre-raster cull accessors. Absolute previous-frame tile count above which the prune fires. 0 disables.</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_hotspot_min_radius_px()</code></pre></td>
      <td>Raw minor-axis screen radius threshold for pruning splats from hot tiles.</td>
    </tr>
    <tr>
      <td><pre><code>gs_is_hotspot_cull_enabled()</code></pre></td>
      <td>Whether the hotspot-pressure cull is enabled for this frame.</td>
    </tr>
  </tbody>
</table>

### Uniform Blocks

#### RenderParams (params)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>view_matrix</code></pre></td>
      <td><pre><code>mat4</code></pre></td>
      <td>World-to-view matrix (may include instance transform)</td>
    </tr>
    <tr>
      <td><pre><code>inv_view_matrix</code></pre></td>
      <td><pre><code>mat4</code></pre></td>
      <td>Inverse of view_matrix</td>
    </tr>
    <tr>
      <td><pre><code>projection_matrix</code></pre></td>
      <td><pre><code>mat4</code></pre></td>
      <td>View-to-clip projection matrix</td>
    </tr>
    <tr>
      <td><pre><code>inv_projection_matrix</code></pre></td>
      <td><pre><code>mat4</code></pre></td>
      <td>Clip-to-view inverse projection matrix (GS render projection)</td>
    </tr>
    <tr>
      <td><pre><code>viewport_size</code></pre></td>
      <td><pre><code>vec2</code></pre></td>
      <td>Viewport size in pixels</td>
    </tr>
    <tr>
      <td><pre><code>tile_count</code></pre></td>
      <td><pre><code>vec2</code></pre></td>
      <td>Tile grid dimensions (x,y)</td>
    </tr>
    <tr>
      <td><pre><code>total_gaussians</code></pre></td>
      <td><pre><code>uint</code></pre></td>
      <td>Total splats in the source buffer</td>
    </tr>
    <tr>
      <td><pre><code>visible_gaussians</code></pre></td>
      <td><pre><code>uint</code></pre></td>
      <td>Visible splats after culling</td>
    </tr>
    <tr>
      <td><pre><code>near_plane</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Camera near plane distance</td>
    </tr>
    <tr>
      <td><pre><code>far_plane</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Camera far plane distance</td>
    </tr>
    <tr>
      <td><pre><code>debug_flags</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: tile_grid_or_bounds, y: collect_raster_stats, z: overflow_tiles, w: projection_issues</td>
    </tr>
    <tr>
      <td><pre><code>debug_overlay_opacity</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Blend strength for debug overlays</td>
    </tr>
    <tr>
      <td><pre><code>opacity_multiplier</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Global opacity scaling factor</td>
    </tr>
    <tr>
      <td><pre><code>_pad0</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Explicit padding to align camera_position to 16 bytes (std140)</td>
    </tr>
    <tr>
      <td><pre><code>_pad1</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>std140 padding</td>
    </tr>
    <tr>
      <td><pre><code>camera_position</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>xyz used, w reserved</td>
    </tr>
    <tr>
      <td><pre><code>alpha_floor</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Minimum alpha when force_solid_coverage is enabled</td>
    </tr>
    <tr>
      <td><pre><code>force_solid_coverage</code></pre></td>
      <td><pre><code>uint</code></pre></td>
      <td>Nonzero enforces alpha_floor for any covered pixel</td>
    </tr>
    <tr>
      <td><pre><code>overlap_record_count</code></pre></td>
      <td><pre><code>uint</code></pre></td>
      <td>Total overlap records in the sorted buffer</td>
    </tr>
    <tr>
      <td><pre><code>_pad_overlap</code></pre></td>
      <td><pre><code>uint</code></pre></td>
      <td>std140 padding</td>
    </tr>
    <tr>
      <td><pre><code>cull_far_tolerance</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Extra tolerance past the far plane for culling</td>
    </tr>
    <tr>
      <td><pre><code>tiny_splat_screen_radius</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Drop splats smaller than this pixel radius</td>
    </tr>
    <tr>
      <td><pre><code>max_conic_aspect</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Max aspect ratio for conic clamping (lower = less stretching)</td>
    </tr>
    <tr>
      <td><pre><code>low_pass_filter</code></pre></td>
      <td><pre><code>float</code></pre></td>
      <td>Mip-Splatting projection low-pass / cov2d diagonal floor; also aligns jacobian_diag_flags (vec4) to 16 bytes</td>
    </tr>
    <tr>
      <td><pre><code>jacobian_diag_flags</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Jacobian diagnostic toggles: x=bypass_radius_depth_floor, y=bypass_j_col2_clamp, z=invert_j_col2_sign, w=reserved</td>
    </tr>
    <tr>
      <td><pre><code>debug_overlay_flags</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Debug overlay flags: x=overlay_mode (0=off, 1=density_heatmap, 2=shadow_opacity), y=depth_visualization, z=projection_z_mismatch, w=white_albedo_lighting_isolation</td>
    </tr>
    <tr>
      <td><pre><code>sh_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Spherical Harmonics configuration: x=sh_bands (0-3), y=amortization_divisor, z=amortization_phase, w=force_full_update sh_bands: 0=DC only, 1=1st order, 2=2nd order, 3=3rd order (full)</td>
    </tr>
    <tr>
      <td><pre><code>sh_decode_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>SH decode configuration (reserved for legacy/debug; runtime decode now comes from per-gaussian metadata).</td>
    </tr>
    <tr>
      <td><pre><code>opacity_culling_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Opacity-aware culling (FlashGS optimization): x=enabled (1.0=true), y=visibility_threshold (tau), z=alpha_clip (optional per-splat hard cull at projection), w=reserved When enabled, splat radii are calculated as: r = sqrt(2 * ln(alpha/tau) * lambda_max) This reduces tile-Gaussian pairs by ~94% for low-opacity splats</td>
    </tr>
    <tr>
      <td><pre><code>lod_blend_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>LOD blending configuration (LODGE technique): x=lod_blend_factor (0-1, global blend factor for the frame) y=lod_blend_enabled (0 or 1) z=lod_blend_distance (world units) w=reserved</td>
    </tr>
    <tr>
      <td><pre><code>distance_cull_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Distance-based culling configuration: x=start_distance, y=max_cull_rate, z=enabled, w=overlap_keep_ratio overlap_keep_ratio is used by global-sort binning to thin overlap records deterministically when the overlap budget is saturated.</td>
    </tr>
    <tr>
      <td><pre><code>color_grading_primary</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Color grading parameters (aligned to vec4): x=enabled (0/1), y=exposure, z=contrast, w=saturation</td>
    </tr>
    <tr>
      <td><pre><code>color_grading_secondary</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x=temperature, y=tint, z=hue_shift (degrees), w=reserved</td>
    </tr>
    <tr>
      <td><pre><code>lighting_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Lighting configuration: x=direct_light_scale, y=indirect_sh_scale, z=enable_direct_lighting, w=normal_mode (0=depth gradients, 1=view dir, 2=depth gradients with fallback)</td>
    </tr>
    <tr>
      <td><pre><code>shadow_strength</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Shadow configuration: x=shadow_strength (0..1), yzw=reserved</td>
    </tr>
    <tr>
      <td><pre><code>shadow_bias_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Shadow receiver bias configuration: x=receiver_bias_scale (per-splat radius multiplier), y=receiver_bias_min, z=receiver_bias_max (0=disabled), w=reserved</td>
    </tr>
    <tr>
      <td><pre><code>lighting_mode</code></pre></td>
      <td><pre><code>uvec4</code></pre></td>
      <td>Lighting mode: x=direct_lighting_mode (0=resolve adds direct, 1=per-splat binning bakes direct), yzw=reserved. The two passes own DISJOINT modes; there is no "both" mode.</td>
    </tr>
    <tr>
      <td><pre><code>light_counts</code></pre></td>
      <td><pre><code>uvec4</code></pre></td>
      <td>Light counts: x=omni_light_count, y=spot_light_count, z=cluster_enabled, w=light_mask</td>
    </tr>
    <tr>
      <td><pre><code>cluster_config</code></pre></td>
      <td><pre><code>uvec4</code></pre></td>
      <td>Cluster configuration: x=cluster_shift, y=cluster_width, z=cluster_type_size, w=max_cluster_element_count_div_32</td>
    </tr>
    <tr>
      <td><pre><code>instance_rotation_inv_col0</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Instance rotation inverse for SH view direction correction (mat3 stored as 3 vec4s for std140).</td>
    </tr>
    <tr>
      <td><pre><code>wind_dir_strength</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Global procedural wind configuration: wind_dir_strength: xyz = direction (world), w = displacement strength (meters)</td>
    </tr>
    <tr>
      <td><pre><code>wind_time_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>wind_time_config: x = time_seconds, y = temporal_frequency, z = spatial_frequency, w = enabled (0/1)</td>
    </tr>
    <tr>
      <td><pre><code>effector_meta</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Bounded sphere effector payload: effector_meta: x = active_count, y = max_supported_count, z = requested_count, w = scene_binding_present</td>
    </tr>
    <tr>
      <td><pre><code>hotspot_cull_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Hotspot-aware pre-raster cull (deterministic, shared by COUNT and EMIT): x = hotspot_pressure_threshold (absolute previous-frame tile count; 0 disables) y = hotspot_min_radius_px (raw minor-axis radius threshold for pruning) z,w = reserved</td>
    </tr>
    <tr>
      <td><pre><code>scene_depth_clip_config</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>Per-splat scene-depth clip (mesh<->splat mid-cloud interleave, compositing slice D): scene_depth_clip_config:  x = enabled (0/1), y = eps_norm (composite depth epsilon normalized into the GS linear-depth space), z/w = the scene projection's depth_linearize mul/add (perspective). scene_depth_clip_config2: x = scene z_near, y = scene z_far (ortho linearize pair), z = is_orthogonal (0/1), w = reserved (explicit pad — this block is byte-hashed on the CPU; never leave it uninitialized).</td>
    </tr>
  </tbody>
</table>


## gs_sh_binning.glsl

`modules/gaussian_splatting/shaders/includes/gs_sh_binning.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gaussian_get_first_order_count(uint meta)</code></pre></td>
      <td>Read the number of first-order SH coefficients encoded in metadata.</td>
    </tr>
    <tr>
      <td><pre><code>gaussian_get_high_order_count(uint meta)</code></pre></td>
      <td>Read the number of higher-order SH coefficients encoded in metadata.</td>
    </tr>
    <tr>
      <td><pre><code>gaussian_get_encoded_count(uint meta)</code></pre></td>
      <td>Read the total number of packed SH coefficients stored in metadata.</td>
    </tr>
    <tr>
      <td><pre><code>gaussian_get_sh_encoding(uint meta)</code></pre></td>
      <td>Read the SH storage format identifier from metadata.</td>
    </tr>
    <tr>
      <td><pre><code>decode_rgb9e5(uint packed)</code></pre></td>
      <td>Decode one RGB9E5-packed SH coefficient triplet to linear RGB.</td>
    </tr>
    <tr>
      <td><pre><code>compute_sh_basis(vec3 dir, uint max_band, out float basis[16])</code></pre></td>
      <td>SH sign convention (ISSUE-038): Condon-Shortley phase included. This evaluation uses the real spherical harmonics basis with the Condon-Shortley (CS) phase factor.  Matches ply_loader.cpp import convention and gaussian_splat_common_inc.glsl.  See those files for full documentation. Compute SH basis functions up to the specified band level basis[0] = DC (l=0) basis[1-3] = 1st order (l=1) basis[4-8] = 2nd order (l=2) basis[9-15] = 3rd order (l=3)</td>
    </tr>
    <tr>
      <td><pre><code>compute_sh_basis_1st_order(vec3 dir, out float basis[4])</code></pre></td>
      <td>Legacy 1st order basis for backwards compatibility</td>
    </tr>
    <tr>
      <td><pre><code>evaluate_sh_with_bands(Gaussian g, vec3 view_dir, uint sh_band_level)</code></pre></td>
      <td>Evaluate SH color with configurable band level sh_band_level: 0=DC only, 1=1st order, 2=2nd order, 3=3rd order</td>
    </tr>
    <tr>
      <td><pre><code>evaluate_sh_1st_order(Gaussian g, vec3 view_dir)</code></pre></td>
      <td>Legacy evaluate function that uses 1st order only (for backwards compatibility)</td>
    </tr>
  </tbody>
</table>


## gs_sort_key.glsl

`modules/gaussian_splatting/shaders/includes/gs_sort_key.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gs_float_to_sortable_uint(float value)</code></pre></td>
      <td>64-bit key layout: hi = sortable depth, lo = tie-break.</td>
    </tr>
    <tr>
      <td><pre><code>gs_pack_sort_key64(float depth, uint tie_break)</code></pre></td>
      <td>Pack depth and tie-break data into a 64-bit sort key.</td>
    </tr>
  </tbody>
</table>


## painterly_common.glsl

`modules/gaussian_splatting/shaders/includes/painterly_common.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>painterly_quaternion_to_matrix(vec4 q)</code></pre></td>
      <td>Convert a quaternion rotation to a 3x3 rotation matrix.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_scale_matrix(vec3 scale)</code></pre></td>
      <td>Build a diagonal scale matrix from per-axis scale values.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_build_covariance(mat3 rotation_matrix, vec3 scale)</code></pre></td>
      <td>Build a covariance matrix from rotation and scale in 3D space.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_build_covariance(vec3 scale, vec4 rotation)</code></pre></td>
      <td>Build a covariance matrix from scale and quaternion inputs.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_project_gaussian(mat3 view_matrix, mat3 covariance_3d)</code></pre></td>
      <td>Project a 3D covariance into view space and derive the 2D conic form.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_compute_radius(const PainterlyConicData data, float sigma_multiplier)</code></pre></td>
      <td>Compute a screen-space radius from projected covariance.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_gaussian_power(vec2 uv, vec3 conic)</code></pre></td>
      <td>Evaluate the quadratic form for a projected Gaussian at a pixel offset.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_gaussian_alpha(float opacity, float power)</code></pre></td>
      <td>Convert Gaussian power into a clamped alpha contribution.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_hash_scalar(vec3 value)</code></pre></td>
      <td>Hash a 3D value to a stable scalar in [0, 1).</td>
    </tr>
    <tr>
      <td><pre><code>painterly_hash_vector(vec3 value)</code></pre></td>
      <td>Hash a 3D value to a stable 2D seed vector.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_safe_normalize(vec3 v, vec3 fallback)</code></pre></td>
      <td>Normalize a vector with a fallback for near-zero length inputs.</td>
    </tr>
  </tbody>
</table>


## painterly_features.glsl

`modules/gaussian_splatting/shaders/includes/painterly_features.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>painterly_apply_palette_quantization(vec3 color, vec2 seeds)</code></pre></td>
      <td>Snap a color toward the nearest palette entry with optional jitter.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_apply_brush_modulation(vec2 uv, vec2 seeds)</code></pre></td>
      <td>Turn brush UV and noise seeds into a soft radial brush mask.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_color_temperature(float kelvin)</code></pre></td>
      <td>Approximate a Kelvin temperature as an RGB tint.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_apply_temperature(vec3 color, float kelvin, float strength)</code></pre></td>
      <td>Blend a color toward a Kelvin-based tint by the requested strength.</td>
    </tr>
    <tr>
      <td><pre><code>cel_shade(vec3 color, vec3 normal, vec3 light_dir, int bands)</code></pre></td>
      <td>Quantize diffuse lighting into flat cel-shading bands.</td>
    </tr>
    <tr>
      <td><pre><code>rim_light(vec3 color, vec3 normal, vec3 view_dir, float power)</code></pre></td>
      <td>Compute a view-dependent rim factor scaled by the power exponent.</td>
    </tr>
    <tr>
      <td><pre><code>gooch_shade_with_dir(vec3 cool_color, vec3 warm_color, vec3 normal, vec3 light_dir)</code></pre></td>
      <td>Blend between cool and warm colors using a signed light-facing term.</td>
    </tr>
    <tr>
      <td><pre><code>gooch_shade(vec3 cool_color, vec3 warm_color, vec3 normal)</code></pre></td>
      <td>Gooch-shade using the first configured painterly light direction.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_mix(vec3 base, vec3 light, float brush_texture)</code></pre></td>
      <td>Mix base and lit colors according to brush influence and texture.</td>
    </tr>
    <tr>
      <td><pre><code>painterly_apply_stylized_lighting(vec3 albedo, vec3 normal_vs, vec3 view_dir_vs)</code></pre></td>
      <td>Apply the selected painterly lighting style to a view-space fragment.</td>
    </tr>
  </tbody>
</table>

### Uniform Blocks

#### PainterlyPalette (painterly_palette)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>params</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: count, y: blend strength, z: noise strength, w: preserve luminance (>0.5)</td>
    </tr>
  </tbody>
</table>

#### PainterlyBrush (painterly_brush)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>params0</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: scale, y: softness, z: anisotropy, w: rotation jitter</td>
    </tr>
    <tr>
      <td><pre><code>params1</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: texture noise strength, yzw: reserved</td>
    </tr>
  </tbody>
</table>

#### PainterlyLighting (painterly_lighting)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>ambient_color</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>rgb: ambient tint, a: ambient intensity</td>
    </tr>
    <tr>
      <td><pre><code>shadow_color</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>rgb: artistic shadow tint, a: shadow strength</td>
    </tr>
    <tr>
      <td><pre><code>highlight_color</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>rgb: highlight tint, a: highlight strength</td>
    </tr>
    <tr>
      <td><pre><code>rim_color_power</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>rgb: rim tint, a: rim power</td>
    </tr>
    <tr>
      <td><pre><code>style_params0</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: shading style, y: cel band count, z: painterly mix strength, w: brush influence</td>
    </tr>
    <tr>
      <td><pre><code>style_params1</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: rim blend, y: color temperature (K), z: temperature strength, w: temporal stability</td>
    </tr>
    <tr>
      <td><pre><code>style_params2</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: gooch cool mix, y: gooch warm mix, z: cel softness, w: unused</td>
    </tr>
    <tr>
      <td><pre><code>light_control</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x: light count, y: global intensity, z/w: reserved</td>
    </tr>
  </tbody>
</table>


## platform_compat.glsl

`modules/gaussian_splatting/shaders/includes/platform_compat.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gs_exp_fast(float x)</code></pre></td>
      <td>Fast exp approximation using Schraudolph's bit manipulation method exp(x) ≈ 2^(x / ln(2)) represented as IEEE 754 float bits</td>
    </tr>
  </tbody>
</table>


## quantization_dequant.glsl

`modules/gaussian_splatting/shaders/includes/quantization_dequant.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>sanitize_quant_bits(uint bits, uint min_bits, uint max_bits)</code></pre></td>
      <td>Clamp quantization bit depth to the supported range.</td>
    </tr>
    <tr>
      <td><pre><code>compute_inv_quant_max(uint bits, uint min_bits, uint max_bits)</code></pre></td>
      <td>Compute the reciprocal of the maximum quantized integer for a bit depth.</td>
    </tr>
    <tr>
      <td><pre><code>extract_quantized_position(uvec2 position_chunk)</code></pre></td>
      <td>Extract quantized position components from packed data</td>
    </tr>
    <tr>
      <td><pre><code>extract_chunk_id(uvec2 position_chunk)</code></pre></td>
      <td>Extract chunk ID from packed position data</td>
    </tr>
    <tr>
      <td><pre><code>extract_quantized_scale(uint scale_lo, uint scale_hi)</code></pre></td>
      <td>Extract quantized scale components from packed data (scalar version for struct layout)</td>
    </tr>
    <tr>
      <td><pre><code>extract_quantized_scale(uvec2 scale_area)</code></pre></td>
      <td>Legacy overload for uvec2 (backward compatibility)</td>
    </tr>
    <tr>
      <td><pre><code>extract_area(uint scale_hi)</code></pre></td>
      <td>Extract area from packed scale/area data (scalar version)</td>
    </tr>
    <tr>
      <td><pre><code>extract_area(uvec2 scale_area)</code></pre></td>
      <td>Legacy overload for uvec2</td>
    </tr>
    <tr>
      <td><pre><code>extract_rotation(uint rotation_lo, uint rotation_hi)</code></pre></td>
      <td>Extract rotation quaternion from packed data (scalar version for struct layout)</td>
    </tr>
    <tr>
      <td><pre><code>extract_rotation(uvec2 rotation_packed)</code></pre></td>
      <td>Legacy overload for uvec2 (backward compatibility)</td>
    </tr>
    <tr>
      <td><pre><code>dequantize_position(uvec3 quantized, ChunkQuantization chunk)</code></pre></td>
      <td>Dequantize position using asset-local chunk bounds.</td>
    </tr>
    <tr>
      <td><pre><code>dequantize_scale(uvec3 quantized, ChunkQuantization chunk)</code></pre></td>
      <td>Dequantize scale using asset-local chunk bounds.</td>
    </tr>
    <tr>
      <td><pre><code>extract_normal(uint normal_xy, uint normal_z_stroke)</code></pre></td>
      <td>Extract normal from packed data</td>
    </tr>
    <tr>
      <td><pre><code>extract_stroke_age(uint normal_z_stroke)</code></pre></td>
      <td>Extract stroke age from packed normal data</td>
    </tr>
    <tr>
      <td><pre><code>get_max_position_error(ChunkQuantization chunk)</code></pre></td>
      <td>Utility: Compute quantization error bounds Calculate maximum position quantization error for a chunk</td>
    </tr>
    <tr>
      <td><pre><code>get_max_scale_error(ChunkQuantization chunk)</code></pre></td>
      <td>Calculate maximum scale quantization error for a chunk</td>
    </tr>
  </tbody>
</table>


## tile_projection_common.glsl

`modules/gaussian_splatting/shaders/includes/tile_projection_common.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gs_pack_screen_xy(vec2 screen_pos)</code></pre></td>
      <td>Pack screen-space position into two half-floats.</td>
    </tr>
    <tr>
      <td><pre><code>gs_pack_normal_z_opacity_flags(vec3 normal, float opacity, uint flags)</code></pre></td>
      <td>Pack the Z normal component (low 16 bits, half) plus opacity (unorm8) and 8 bits of flags (high 16 bits) into one 32-bit word. Replaces the separate gs_pack_normal_zw + depth-word opacity: the high half of the normal-z word was unused, and reclaiming it frees data[1] to carry raw fp32 depth (see header comment).</td>
    </tr>
    <tr>
      <td><pre><code>gs_pack_color_r11g11b10(vec3 color)</code></pre></td>
      <td>Pack linear RGB into the tile color payload format.</td>
    </tr>
    <tr>
      <td><pre><code>gs_pack_normal_xy(vec3 normal)</code></pre></td>
      <td>Pack the X/Y normal components into one 32-bit word.</td>
    </tr>
    <tr>
      <td><pre><code>gs_pack_conic_y_and_index(float conic_y, uint global_idx)</code></pre></td>
      <td>Legacy function kept for API compatibility. IMPORTANT: this packs `global_idx` into the high 16 bits and the rasterizer (tile_raster_common.glsl, `if (stored_global_idx != sorted_idx) continue;`) will silently reject any visible splat whose actual global_idx exceeds UINT16_MAX = 65535. Callers must ensure `enable_packed_stage_data` is only used when the packed-stage payload count is <= 65535. Runtime enforcement is the TileRenderer disable gate (renderer/tile_renderer.cpp ~ line 415), which has the scene payload count; PipelineFeatureSet only carries the shared PACKED_STAGE_MAX_TOTAL_SPLATS = 65535 capability constant. The GS_PACKED_STAGE_DATA define is emitted only after the TileRenderer gate. Do not relax these without redesigning the packed payload to carry a full 32-bit global_idx.</td>
    </tr>
    <tr>
      <td><pre><code>gs_unpack_screen_xy(uint packed)</code></pre></td>
      <td>Unpack the packed screen-space position.</td>
    </tr>
    <tr>
      <td><pre><code>gs_unpack_opacity_flags(uint packed, out float opacity, out uint flags)</code></pre></td>
      <td>Unpack opacity and flags from the high 16 bits of the normal-z word.</td>
    </tr>
    <tr>
      <td><pre><code>gs_unpack_color_r11g11b10(uint packed)</code></pre></td>
      <td>Unpack the tile color payload back into linear RGB.</td>
    </tr>
    <tr>
      <td><pre><code>gs_unpack_normal(uint packed_xy, uint packed_zw)</code></pre></td>
      <td>Unpack the normal payload back into a 3D normal vector.</td>
    </tr>
    <tr>
      <td><pre><code>gs_unpack_conic_y_and_index(uint packed, out float conic_y, out uint global_idx)</code></pre></td>
      <td>Legacy function kept for API compatibility</td>
    </tr>
    <tr>
      <td><pre><code>tile_projection_index(uint tile_index, uint slot_index)</code></pre></td>
      <td>Compute the linear index for a tile/slot pair in the projection buffer.</td>
    </tr>
  </tbody>
</table>


## tile_raster_common.glsl

`modules/gaussian_splatting/shaders/includes/tile_raster_common.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gs_scene_clip_limit(vec2 pixel_center)</code></pre></td>
      <td>Once-per-pixel scene-depth clip limit in the payload's normalized linear-depth space. A splat whose fp32 payload linear_depth exceeds this limit is behind the opaque scene geometry at this pixel and must contribute nothing. Conversion chain (one fetch, NEAREST, size-mapped like the Slice-A composite fix): raw NDC scene depth -> positive view-space z via the SCENE projection's linearize mul/add (perspective) or near/far (ortho) — the exact formulas the composite uses in viewport_blit.glsl — then normalized with the GS raster's near/far pair (params.near_plane/far_plane), because THAT pair defined the payload linear_depth at binning time (tile_binning.glsl). eps_norm is the composite's view-space depth-epsilon contract pre-divided by (far - near). Background bypass: a raw depth equal to the far-plane clear value (0.0 or 1.0 depending on reversed-Z, matching is_scene_background_depth's raw check in the composite) means "no mesh here" -> no clip. This is exact regardless of any near/far mismatch between the GS and scene projections.</td>
    </tr>
    <tr>
      <td><pre><code>gs_dither_noise(vec2 frag_coord)</code></pre></td>
      <td>Simple hash-based noise for dithering (spatially varying) Uses fragment position to generate pseudo-random value in [-0.5, 0.5]</td>
    </tr>
    <tr>
      <td><pre><code>gs_dither_noise_rgb(vec2 frag_coord)</code></pre></td>
      <td>Generate RGB dither noise using different offsets for each channel This breaks up color banding from quantization</td>
    </tr>
    <tr>
      <td><pre><code>gs_apply_color_dither(vec3 color, vec2 frag_coord)</code></pre></td>
      <td>Apply dithering to a color to reduce quantization banding Uses flat dithering (not scaled by color) for consistent banding reduction across all tones</td>
    </tr>
    <tr>
      <td><pre><code>gs_read_sorted_value(uint local_index, uint range_start)</code></pre></td>
      <td>Read a sorted splat index from shared memory or the backing buffer.</td>
    </tr>
    <tr>
      <td><pre><code>gs_read_projected_gaussian(uint local_index, uint sorted_idx)</code></pre></td>
      <td>Read a projected Gaussian payload from shared memory or the backing buffer.</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_visible_gaussian_count()</code></pre></td>
      <td>Return the number of visible Gaussians in the current dispatch.</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_sorted_value_capacity()</code></pre></td>
      <td>Return the actual capacity of the sorted overlap-value buffer bound for this raster dispatch. This is the final safety boundary before values[] reads.</td>
    </tr>
    <tr>
      <td><pre><code>gs_get_clamped_overlap_record_count()</code></pre></td>
      <td>The prefix scan should already clamp indirect_dispatch.element_count to the overlap buffer capacity, but the rasterizer must not trust a GPU-produced count blindly. Clamp again at the consumer so corrupt or stale indirect payloads cannot drive out-of-bounds reads from sorted_values.values[].</td>
    </tr>
  </tbody>
</table>


## painterly_composite.frag.glsl

`modules/gaussian_splatting/shaders/painterly_composite.frag.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>linearize_scene_depth(float raw_depth)</code></pre></td>
      <td>Convert normalized scene depth to comparable view-space depth.</td>
    </tr>
    <tr>
      <td><pre><code>sanitize_view_depth(float depth_value)</code></pre></td>
      <td>Clamp invalid depth values to a sentinel for comparisons.</td>
    </tr>
    <tr>
      <td><pre><code>is_scene_background_depth(float raw_scene_depth, float scene_view_depth)</code></pre></td>
      <td>Detect whether the sampled scene depth corresponds to the background clear value.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Fragment entry point for the painterly composite pass.</td>
    </tr>
  </tbody>
</table>


## painterly_composite.glsl

`modules/gaussian_splatting/shaders/painterly_composite.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Vertex entry point for the fullscreen composite triangle.</td>
    </tr>
    <tr>
      <td><pre><code>linearize_scene_depth(float raw_depth)</code></pre></td>
      <td>Convert normalized scene depth to comparable view-space depth.</td>
    </tr>
    <tr>
      <td><pre><code>sanitize_view_depth(float depth_value)</code></pre></td>
      <td>Clamp invalid depth values to a sentinel for comparisons.</td>
    </tr>
    <tr>
      <td><pre><code>is_scene_background_depth(float raw_scene_depth, float scene_view_depth)</code></pre></td>
      <td>Detect whether the sampled scene depth corresponds to the background clear value.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Fragment entry point for the fullscreen composite pass.</td>
    </tr>
  </tbody>
</table>


## painterly_composite.vert.glsl

`modules/gaussian_splatting/shaders/painterly_composite.vert.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Vertex entry point for the fullscreen composite triangle.</td>
    </tr>
  </tbody>
</table>


## painterly_resolve.glsl

`modules/gaussian_splatting/shaders/painterly_resolve.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>hash(vec2 uv)</code></pre></td>
      <td>Generate a repeatable pseudo-random value from UV coordinates.</td>
    </tr>
    <tr>
      <td><pre><code>layered_noise(vec2 uv, int octaves)</code></pre></td>
      <td>Accumulate layered hash noise for painterly modulation.</td>
    </tr>
    <tr>
      <td><pre><code>apply_palette(vec3 color)</code></pre></td>
      <td>Apply the active painterly palette transformation.</td>
    </tr>
    <tr>
      <td><pre><code>apply_density_response(vec3 color, float alpha)</code></pre></td>
      <td>Adjust color response based on splat density and alpha.</td>
    </tr>
    <tr>
      <td><pre><code>evaluate_painterly(vec2 uv, vec4 base_sample)</code></pre></td>
      <td>Evaluate painterly post-processing for a single pixel sample.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Compute entry point for painterly resolve output.</td>
    </tr>
  </tbody>
</table>


## sobel_outline.glsl

`modules/gaussian_splatting/shaders/sobel_outline.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>sample_color(vec2 uv, vec2 offset)</code></pre></td>
      <td>Sample a neighboring scene color for Sobel edge detection.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Compute outline intensity from Sobel gradients.</td>
    </tr>
  </tbody>
</table>


## tile_binning.glsl

`modules/gaussian_splatting/shaders/tile_binning.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>gs_get_visible_gaussian_count()</code></pre></td>
      <td>Return the number of visible Gaussians scheduled for this dispatch.</td>
    </tr>
    <tr>
      <td><pre><code>gs_tile_intersects_projected_ellipse(vec2 center, vec3 conic, float sigma2, int tx, int ty)</code></pre></td>
      <td>Exact convex-quadratic minimum test against a tile rectangle. Returns true if the projected ellipse intersects the tile.</td>
    </tr>
    <tr>
      <td><pre><code>gs_build_quantized_sh_metadata(uint encoded_total, bool dc_linear_rgb)</code></pre></td>
      <td>Pack quantized spherical-harmonic metadata for the renderer.</td>
    </tr>
    <tr>
      <td><pre><code>project_gaussian_2d(Gaussian g, out vec2 screen_pos, out mat2 cov2d, out float linear_depth, out float raw_min_radius, out float alpha_rescale)</code></pre></td>
      <td>Project a Gaussian into screen space and derive its 2D covariance. `alpha_rescale` is the Mip-Splatting α-rescale companion (Yu et al. 2024 §3.3). The additive low-pass term inflates the projected splat; without rescaling alpha by sqrt(det(Σ_raw) / det(Σ_filtered)) the dilation becomes pure fattening, producing a uniform soft halo (matched the symptom on commit adc75e5ca8). Defaults to 1.0 on early-return paths so non-rendering splats are unaffected.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Compute entry point for the active tile binning pass.</td>
    </tr>
  </tbody>
</table>


## tile_prefix_scan.glsl

`modules/gaussian_splatting/shaders/tile_prefix_scan.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Compute pass 1: local exclusive scan and workgroup totals.</td>
    </tr>
    <tr>
      <td><pre><code>read_pass2_source(uint idx)</code></pre></td>
      <td>Read the current prefix-scan source buffer for pass 2.</td>
    </tr>
    <tr>
      <td><pre><code>write_pass2_dest(uint idx, uint value)</code></pre></td>
      <td>Write the current prefix-scan destination buffer for pass 2.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Compute pass 2: multi-level scan or buffer copy for workgroup offsets.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Pass 3: add workgroup offsets into base ranges.</td>
    </tr>
  </tbody>
</table>

### Uniform Blocks

#### PrefixParams (params)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>global_sort_capacity</code></pre></td>
      <td><pre><code>uint</code></pre></td>
      <td>Used for overflow detection in Pass 3</td>
    </tr>
  </tbody>
</table>


## tile_rasterizer.glsl

`modules/gaussian_splatting/shaders/tile_rasterizer.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Vertex entry point for the fullscreen raster quad.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Fragment entry point for tile rasterization and compositing.</td>
    </tr>
  </tbody>
</table>


## tile_rasterizer_compute.glsl

`modules/gaussian_splatting/shaders/tile_rasterizer_compute.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Compute entry point for tile-local batched rasterization.</td>
    </tr>
  </tbody>
</table>


## tile_resolve.glsl

`modules/gaussian_splatting/shaders/tile_resolve.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>sample_input_color(ivec2 coord, vec2 uv)</code></pre></td>
      <td>Sample the resolved input color buffer with optional texel fetch.</td>
    </tr>
    <tr>
      <td><pre><code>sample_input_depth(ivec2 coord, vec2 uv)</code></pre></td>
      <td>Sample the resolved input depth buffer with optional texel fetch.</td>
    </tr>
    <tr>
      <td><pre><code>sample_input_normal(ivec2 coord, vec2 uv)</code></pre></td>
      <td>Sample the resolved input normal buffer with optional texel fetch.</td>
    </tr>
    <tr>
      <td><pre><code>sanitize_linear_depth(float depth_value)</code></pre></td>
      <td>Clamp invalid linear depth values to a safe fallback.</td>
    </tr>
    <tr>
      <td><pre><code>compute_feather_weight(ivec2 coord)</code></pre></td>
      <td>Compute edge feathering for the current tile.</td>
    </tr>
    <tr>
      <td><pre><code>reconstruct_view_pos(mat4 inv_proj, vec2 screen_uv, float linear_depth, float z_near, float z_far, bool is_ortho)</code></pre></td>
      <td>Reconstruct a view-space position from screen UV and linear depth.</td>
    </tr>
    <tr>
      <td><pre><code>apply_tile_debug_overlay(vec4 color, ivec2 coord)</code></pre></td>
      <td>Overlay tile boundaries when tile debug visualization is enabled.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Compute entry point for tile resolve and final shading.</td>
    </tr>
  </tbody>
</table>


## viewport_blit.glsl

`modules/gaussian_splatting/shaders/viewport_blit.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>srgb_to_linear(vec3 color)</code></pre></td>
      <td>Fast sRGB approximations using polynomial/sqrt instead of pow() These avoid expensive pow() calls while maintaining good accuracy Max error ~0.4% which is imperceptible</td>
    </tr>
    <tr>
      <td><pre><code>linear_to_srgb(vec3 color)</code></pre></td>
      <td>Convert linear color to sRGB for final viewport output.</td>
    </tr>
    <tr>
      <td><pre><code>linearize_scene_depth(float raw_depth)</code></pre></td>
      <td>Convert raw scene depth to linear view-space depth.</td>
    </tr>
    <tr>
      <td><pre><code>sanitize_view_depth(float depth_value)</code></pre></td>
      <td>Clamp invalid depth values to a stable far-plane fallback.</td>
    </tr>
    <tr>
      <td><pre><code>is_scene_background_depth(float raw_scene_depth, float scene_view_depth)</code></pre></td>
      <td>Detect background depth samples near the far plane.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Blit the rendered viewport into the final output target.</td>
    </tr>
  </tbody>
</table>


## depth_compute.glsl

`modules/gaussian_splatting/compute/depth_compute.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>quat_rotate(vec4 q, vec3 v)</code></pre></td>
      <td>Rotate vector `v` by quaternion `q`.</td>
    </tr>
    <tr>
      <td><pre><code>gs_sphere_frustum_visible(vec3 position, float radius)</code></pre></td>
      <td>Conservative sphere-frustum visibility test for compute culling.</td>
    </tr>
    <tr>
      <td><pre><code>gs_compute_screen_size(float depth, float radius)</code></pre></td>
      <td>Estimate projected screen-space radius from depth and world-space radius.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Depth compute entry point; emits per-instance depth and screen-size data.</td>
    </tr>
  </tbody>
</table>

### Uniform Blocks

#### Params (params)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>camera_position_ortho</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>xyz = camera position, w = orthographic flag</td>
    </tr>
    <tr>
      <td><pre><code>cull_screen_distance</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x = pixel_scale_y, y = tiny_splat_radius_px, z = min_screen_threshold_px, w = max_distance_sq</td>
    </tr>
    <tr>
      <td><pre><code>cull_frustum_radius</code></pre></td>
      <td><pre><code>vec4</code></pre></td>
      <td>x = radius_multiplier, y = frustum_plane_slack, z = enable_frustum, w = reserved</td>
    </tr>
  </tbody>
</table>


## frustum_cull.glsl

`modules/gaussian_splatting/compute/frustum_cull.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>quat_rotate(vec4 q, vec3 v)</code></pre></td>
      <td>Rotate vector `v` by quaternion `q`.</td>
    </tr>
    <tr>
      <td><pre><code>sphere_frustum_visible(vec3 position, float radius)</code></pre></td>
      <td>Conservative sphere-frustum visibility test for instance culling.</td>
    </tr>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Frustum culling entry point; classifies visible instances for downstream passes.</td>
    </tr>
  </tbody>
</table>


## instance_chunk_dispatch.glsl

`modules/gaussian_splatting/compute/instance_chunk_dispatch.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Build indirect dispatch counts for chunk-level processing.</td>
    </tr>
  </tbody>
</table>

### Uniform Blocks

#### Params (params)

<table>
  <thead>
    <tr>
      <th>Field</th>
      <th>Type</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>max_visible_chunks</code></pre></td>
      <td><pre><code>uint</code></pre></td>
      <td>Uses InstanceDepthParamsGPU.visible_chunk_count slot.</td>
    </tr>
    <tr>
      <td><pre><code>dispatch_group_x</code></pre></td>
      <td><pre><code>uint</code></pre></td>
      <td>Uses InstanceDepthParamsGPU.pad0 slot.</td>
    </tr>
  </tbody>
</table>


## instance_count_clamp.glsl

`modules/gaussian_splatting/compute/instance_count_clamp.glsl`

### Functions

<table>
  <thead>
    <tr>
      <th>Function</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>main()</code></pre></td>
      <td>Clamp indirect instance counts to the configured dispatch budget.</td>
    </tr>
  </tbody>
</table>


Generated by:

```
scripts/generate_shader_docs.py
```
