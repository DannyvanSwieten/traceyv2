import { Component, For, Show, createSignal, createEffect, Accessor } from 'solid-js';
import * as api from '../../lib/api';
import { ImportedAsset } from '../../stores/assets';
import './ResourcesBrowser.css';

type MeshInfo = api.MeshInfo;
type TextureInfo = api.TextureInfo;

type TabType = 'scenes' | 'meshes' | 'textures';

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
      </div>
    </div>
  );
};
