// Human-readable parameter labels.
//
// SOP / VOP params are stored in snake_case (base_color, clearcoat_roughness,
// rotate_euler_deg, …) and the inspectors used to show that raw name, which
// reads poorly. humanizeParamName() turns the stored name into a Title-Case
// label — acronym-aware, with a few explicit overrides for names that
// title-case awkwardly. The raw name stays the source of truth for param
// lookups / hover titles; this is display-only.

// Tokens that should render fully uppercase rather than Title-cased.
const ACRONYMS = new Set([
  'ior', 'uv', 'uv0', 'uv1', 'rgb', 'rgba', 'hdri', 'fov',
  'sss', 'ao', 'id', 'dof', 'aov', 'pbr', '2d', '3d',
]);

// Explicit overrides where simple title-casing reads badly.
const OVERRIDES: Record<string, string> = {
  rotate_euler_deg: 'Rotation',
  material_library_name: 'Material Library',
};

export function humanizeParamName(name: string): string {
  const override = OVERRIDES[name];
  if (override) return override;
  return name
    .split('_')
    .filter((w) => w.length > 0)
    .map((w) =>
      ACRONYMS.has(w.toLowerCase())
        ? w.toUpperCase()
        : w.charAt(0).toUpperCase() + w.slice(1),
    )
    .join(' ');
}
