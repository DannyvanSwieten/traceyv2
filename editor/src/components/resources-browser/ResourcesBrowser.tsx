import { Component, For, Show, createSignal, createEffect, createMemo, Accessor } from 'solid-js';
import * as api from '../../lib/api';
import { ImportedAsset } from '../../stores/assets';
import { cookProfile, NodeCookTimingRow } from '../../stores/cook_profiler';
import { renderStats } from '../../stores/render_stats';
import './ResourcesBrowser.css';

// Render a number with thousands separators. The triangle / BVH-node
// counts get into the millions for dense scenes, so spaces every 3
// digits keep them scannable.
function fmtCount(n: number): string {
  return new Intl.NumberFormat('en-US').format(Math.round(n));
}

type MeshInfo = api.MeshInfo;
type TextureInfo = api.TextureInfo;

type TabType = 'scenes' | 'meshes' | 'textures' | 'profiler';

interface ResourcesBrowserProps {
  assets: () => ImportedAsset[];
  currentAssetPath: Accessor<string | null>;
  onAssetSelect: (asset: ImportedAsset) => void;
  // Pull the asset's contents into the live SOP graph (for glTF: a recursive
  // subnet tree mirroring the node hierarchy). Separate from onAssetSelect
  // so registering an asset stays cheap and the user picks the moment to
  // actually materialise it.
  onAssetLoad: (asset: ImportedAsset) => void;
  onAssetRemove: (id: string) => void;
}

export const ResourcesBrowser: Component<ResourcesBrowserProps> = (props) => {
  const [activeTab, setActiveTab] = createSignal<TabType>('scenes');
  const [meshes, setMeshes] = createSignal<MeshInfo[]>([]);
  const [textures, setTextures] = createSignal<TextureInfo[]>([]);
  const [isLoading, setIsLoading] = createSignal(false);

  // Per-node cook timings from the most recent cook, sorted by ms desc.
  // Rows expose their source uid so a future "click to focus the offender
  // in the canvas" affordance has the information it needs.
  const profilerRows = createMemo<NodeCookTimingRow[]>(() => {
    const p = cookProfile();
    if (!p) return [];
    return p.rows.slice().sort((a, b) => b.ms - a.ms);
  });
  const profilerTotalMs = createMemo(() => cookProfile()?.totalMs ?? 0);

  createEffect(async () => {
    const currentPath = props.currentAssetPath();
    if (currentPath) {
      setIsLoading(true);
      try {
        const [loadedMeshes, loadedTextures] = await Promise.all([
          api.getAllMeshes(),
          api.getAllTextures(),
        ]);
        setMeshes(loadedMeshes);
        setTextures(loadedTextures);
      } catch (error) {
        console.error('Failed to load resources:', error);
        setMeshes([]);
        setTextures([]);
      } finally {
        setIsLoading(false);
      }
    } else {
      setMeshes([]);
      setTextures([]);
    }
  });

  return (
    <div class="resources-browser">
      <div class="resources-header">
        <div class="resources-tabs">
          <button
            class="resources-tab"
            classList={{ 'resources-tab--active': activeTab() === 'scenes' }}
            onClick={() => setActiveTab('scenes')}
          >
            Scenes ({props.assets().length})
          </button>
          <button
            class="resources-tab"
            classList={{ 'resources-tab--active': activeTab() === 'meshes' }}
            onClick={() => setActiveTab('meshes')}
          >
            Meshes ({meshes().length})
          </button>
          <button
            class="resources-tab"
            classList={{ 'resources-tab--active': activeTab() === 'textures' }}
            onClick={() => setActiveTab('textures')}
          >
            Textures ({textures().length})
          </button>
          <button
            class="resources-tab"
            classList={{ 'resources-tab--active': activeTab() === 'profiler' }}
            onClick={() => setActiveTab('profiler')}
            title="Per-node cook times from the most recent cook"
          >
            Profiler{cookProfile() ? ` (${cookProfile()!.totalMs.toFixed(1)}ms)` : ''}
          </button>
        </div>
      </div>
      <div class="resources-content">
        <Show when={activeTab() === 'scenes'}>
          <Show
            when={props.assets().length > 0}
            fallback={
              <div class="resources-empty">
                No assets imported. Use File → Import to add scenes.
              </div>
            }
          >
            <div class="resources-grid">
              <For each={props.assets()}>
                {(asset) => (
                  <div
                    class="resource-item"
                    classList={{
                      'resource-item--active':
                        props.currentAssetPath() === asset.path,
                    }}
                    onDblClick={() => props.onAssetLoad(asset)}
                    title={`${asset.path}\nDouble-click or press Load to drop the hierarchy into the SOP graph.`}
                  >
                    <div class="resource-icon">📦</div>
                    <div class="resource-name">{asset.name}</div>
                    <button
                      class="resource-load"
                      onClick={(e) => {
                        e.stopPropagation();
                        props.onAssetLoad(asset);
                      }}
                      title="Load — add a subnet hierarchy mirroring this glTF to the SOP graph"
                    >
                      Load
                    </button>
                    <button
                      class="resource-remove"
                      onClick={(e) => {
                        e.stopPropagation();
                        props.onAssetRemove(asset.id);
                      }}
                      title="Remove from resources"
                    >
                      ×
                    </button>
                  </div>
                )}
              </For>
            </div>
          </Show>
        </Show>

        <Show when={activeTab() === 'meshes'}>
          <Show when={!isLoading()} fallback={<div class="resources-empty">Loading...</div>}>
            <Show
              when={meshes().length > 0}
              fallback={
                <div class="resources-empty">
                  No meshes loaded. Import a scene first.
                </div>
              }
            >
              <div class="resources-grid">
                <For each={meshes()}>
                  {(mesh) => (
                    <div class="resource-item resource-item--mesh" title={mesh.name}>
                      <div class="resource-icon">🔺</div>
                      <div class="resource-name">{mesh.name || 'Unnamed'}</div>
                      <div class="resource-stats">
                        {mesh.triangle_count} tris
                      </div>
                    </div>
                  )}
                </For>
              </div>
            </Show>
          </Show>
        </Show>

        <Show when={activeTab() === 'textures'}>
          <Show when={!isLoading()} fallback={<div class="resources-empty">Loading...</div>}>
            <Show
              when={textures().length > 0}
              fallback={
                <div class="resources-empty">
                  No textures loaded. Import a scene first.
                </div>
              }
            >
              <div class="resources-grid">
                <For each={textures()}>
                  {(texture) => (
                    <div class="resource-item resource-item--texture" title={texture.id}>
                      <div class="resource-icon">🖼️</div>
                      <div class="resource-name">{texture.id}</div>
                      <div class="resource-stats">
                        {texture.width}x{texture.height}
                      </div>
                    </div>
                  )}
                </For>
              </div>
            </Show>
          </Show>
        </Show>

        <Show when={activeTab() === 'profiler'}>
          {/* Live render stats — broadcast ~4 Hz from the native
              render_tick. Always visible (no fallback) so the user
              can read FPS / triangle counts even before the first
              cook completes. */}
          <Show when={renderStats()}>
            {(stats) => (
              <>
              <div class="profiler-live-stats">
                <div class="profiler-live-stat">
                  <span class="profiler-live-label">FPS</span>
                  <span class="profiler-live-value">{stats().fps.toFixed(1)}</span>
                </div>
                <div class="profiler-live-stat">
                  <span class="profiler-live-label">Triangles</span>
                  <span class="profiler-live-value">{fmtCount(stats().triangles)}</span>
                </div>
                <div class="profiler-live-stat">
                  <span class="profiler-live-label">Instances</span>
                  <span class="profiler-live-value">{fmtCount(stats().instances)}</span>
                </div>
                <div class="profiler-live-stat">
                  <span class="profiler-live-label">BVH nodes</span>
                  <span class="profiler-live-value">{fmtCount(stats().bvh_nodes)}</span>
                </div>
                <div class="profiler-live-stat">
                  <span class="profiler-live-label">Samples</span>
                  <span class="profiler-live-value">
                    {stats().samples}
                    <span class="profiler-live-sub">/ {stats().max_samples}</span>
                  </span>
                </div>
                <div class="profiler-live-stat">
                  <span class="profiler-live-label">PT dispatch</span>
                  <span class="profiler-live-value">{stats().render_time_ms.toFixed(2)}<span class="profiler-live-sub"> ms</span></span>
                </div>
              </div>
              {/* Per-tick wall-clock breakdown. `tick` is the whole
                  render_tick body; the slices below it are the
                  dominant phases. A gap between tick and the sum is
                  unmeasured cost — vsync stall, cook worker
                  contention, or anything else outside the
                  instrumented spans. */}
              <div class="profiler-live-stats profiler-live-stats--row2">
                <div class="profiler-live-stat profiler-live-stat--total">
                  <span class="profiler-live-label">Tick</span>
                  <span class="profiler-live-value">{stats().tick_ms.toFixed(2)}<span class="profiler-live-sub"> ms</span></span>
                </div>
                <div class="profiler-live-stat">
                  <span class="profiler-live-label">Rebuild</span>
                  <span class="profiler-live-value">{stats().rebuild_ms.toFixed(2)}<span class="profiler-live-sub"> ms</span></span>
                </div>
                <div class="profiler-live-stat">
                  <span class="profiler-live-label">Raster</span>
                  <span class="profiler-live-value">{stats().raster_ms.toFixed(2)}<span class="profiler-live-sub"> ms</span></span>
                </div>
                <div class="profiler-live-stat">
                  <span class="profiler-live-label">Present</span>
                  <span class="profiler-live-value">{stats().present_ms.toFixed(2)}<span class="profiler-live-sub"> ms</span></span>
                </div>
              </div>
              </>
            )}
          </Show>
          <Show
            when={profilerRows().length > 0}
            fallback={
              <div class="resources-empty">
                No cook has finished yet. Trigger one by editing the SOP graph
                or loading an asset; this tab fills with per-node timings as
                soon as the worker reports back.
              </div>
            }
          >
            <div class="profiler-summary">
              Total cook: <strong>{profilerTotalMs().toFixed(2)} ms</strong>
              <span class="profiler-summary-rows">
                {profilerRows().length} nodes
              </span>
            </div>
            <div class="profiler-table">
              <div class="profiler-row profiler-row--header">
                <span class="profiler-cell profiler-cell-bar"></span>
                <span class="profiler-cell profiler-cell-ms">ms</span>
                <span class="profiler-cell profiler-cell-name">node</span>
                <span class="profiler-cell profiler-cell-kind">kind</span>
              </div>
              <For each={profilerRows()}>
                {(row) => {
                  const pct = () =>
                    profilerTotalMs() > 0 ? (row.ms / profilerTotalMs()) * 100 : 0;
                  const label = () => row.name || `#${row.node_uid}`;
                  return (
                    <div class="profiler-row" title={`uid ${row.node_uid}${row.parent_node_uid ? ` (inside #${row.parent_node_uid})` : ''}`}>
                      <span class="profiler-cell profiler-cell-bar">
                        <span
                          class="profiler-bar-fill"
                          ref={(el) => el.style.setProperty('--pct', `${pct().toFixed(1)}%`)}
                        />
                      </span>
                      <span class="profiler-cell profiler-cell-ms">{row.ms.toFixed(2)}</span>
                      <span class="profiler-cell profiler-cell-name">{label()}</span>
                      <span class="profiler-cell profiler-cell-kind">{row.kind}</span>
                    </div>
                  );
                }}
              </For>
            </div>
          </Show>
        </Show>
      </div>
    </div>
  );
};
