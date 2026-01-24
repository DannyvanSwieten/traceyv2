import { createSignal, createEffect } from 'solid-js';

export interface ImportedAsset {
  id: string;
  name: string;
  path: string;
  type: 'scene';
  importedAt: number;
}

const STORAGE_KEY = 'tracey-imported-assets';

function loadFromStorage(): ImportedAsset[] {
  try {
    const stored = localStorage.getItem(STORAGE_KEY);
    return stored ? JSON.parse(stored) : [];
  } catch {
    return [];
  }
}

function saveToStorage(assets: ImportedAsset[]) {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(assets));
}

const [assets, setAssets] = createSignal<ImportedAsset[]>(loadFromStorage());

createEffect(() => {
  saveToStorage(assets());
});

export function addAsset(path: string): ImportedAsset {
  const name = path.split('/').pop() || path.split('\\').pop() || 'Untitled';
  const existing = assets().find((a) => a.path === path);
  if (existing) {
    return existing;
  }

  const asset: ImportedAsset = {
    id: crypto.randomUUID(),
    name,
    path,
    type: 'scene',
    importedAt: Date.now(),
  };

  setAssets((prev) => [...prev, asset]);
  return asset;
}

export function removeAsset(id: string) {
  setAssets((prev) => prev.filter((a) => a.id !== id));
}

export function getAssets() {
  return assets;
}

export function clearAssets() {
  setAssets([]);
}
