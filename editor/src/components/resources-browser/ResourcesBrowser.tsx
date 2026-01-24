import { Component, For, Show, createSignal, createEffect, Accessor } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import { ImportedAsset } from '../../stores/assets';
import './ResourcesBrowser.css';

interface MeshInfo {
  name: string;
  vertex_count: number;
  triangle_count: number;
  has_indices: boolean;
  has_normals: boolean;
  has_uvs: boolean;
}

interface TextureInfo {
  id: string;
  width: number;
  height: number;
  channels: number;
  mime_type: string;
}

type TabType = 'scenes' | 'meshes' | 'textures';

interface ResourcesBrowserProps {
  assets: () => ImportedAsset[];
  currentAssetPath: Accessor<string | null>;
  onAssetSelect: (asset: ImportedAsset) => void;
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
          invoke<MeshInfo[]>('get_all_meshes'),
          invoke<TextureInfo[]>('get_all_textures'),
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
                    onDblClick={() => props.onAssetSelect(asset)}
                    title={asset.path}
                  >
                    <div class="resource-icon">📦</div>
                    <div class="resource-name">{asset.name}</div>
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
