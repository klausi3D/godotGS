# Version-Controlled Media

## Purpose

Define how to include images and videos in docs content while keeping media artifacts versioned with the repository.

## Folder Conventions

| Media type | Path |
| --- | --- |
| Images | `docs/assets/images/` |
| Videos | `docs/assets/videos/` |

## Example: Relationship Diagram {#example-image}

<figure class="gs-flow" markdown="1">
  <ol class="gs-flow__steps" aria-label="Source to viewport">
    <li><strong>Source</strong><small>Import or load</small></li>
    <li><strong>Scene</strong><small>Assign the asset</small></li>
    <li><strong>Viewport</strong><small>Render the splats</small></li>
  </ol>
  <figcaption>A diagram describes relationships. Use real editor captures to teach UI actions.</figcaption>
</figure>

## Example: Video

<video controls preload="metadata" width="640">
  <source src="../assets/videos/gaussian-demo.webm" type="video/webm">
  <source src="../assets/videos/gaussian-demo.mp4" type="video/mp4">
  Your browser does not support embedded video playback.
</video>

## Authoring Rules

- Keep docs media under `docs/assets/` so each tag/version serves matching assets.
- Prefer `.webm` + `.mp4` pairs for compatibility.
- Use descriptive file names tied to feature/use case.
- Give each image concise alt text and a caption that explains why the visual is on the page.
- Let standard markdown images stay unwrapped so `glightbox` can provide click-to-expand behavior.
- Use real screenshots for UI actions. Use a diagram only when it clarifies a relationship, branch or data flow; never create a mock editor frame to fill a capture gap.
- For large video files, use Git LFS tracking (`*.mp4`, `*.webm`).
- Keep total media footprint within CI budget gates.
- Follow the [screenshot capture process](../development/screenshot-capture-spec.md) for editor screenshots.

## Git LFS Setup

```bash
git lfs install
git lfs track "*.mp4" "*.webm"
```
