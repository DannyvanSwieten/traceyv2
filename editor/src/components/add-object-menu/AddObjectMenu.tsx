import { Component, createSignal, Show } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import './AddObjectMenu.css';

interface Actor {
  id: number;
  name: string;
  transform: {
    position: { x: number; y: number; z: number };
    rotation: { w: number; x: number; y: number; z: number };
    scale: { x: number; y: number; z: number };
  };
  children: number[];
}

// Primitive type for node graph based creation
type PrimitiveType =
  | { type: 'cube'; size?: number }
  | { type: 'sphere'; radius?: number; segments?: number; rings?: number }
  | { type: 'torus'; major_radius?: number; minor_radius?: number; major_segments?: number; minor_segments?: number }
  | { type: 'plane'; width?: number; depth?: number }
  | { type: 'cylinder'; radius?: number; height?: number; segments?: number }
  | { type: 'cone'; radius?: number; height?: number; segments?: number };

interface AddObjectMenuProps {
  onObjectAdded: (actor: Actor) => void;
}

export const AddObjectMenu: Component<AddObjectMenuProps> = (props) => {
  const [isOpen, setIsOpen] = createSignal(false);
  const [isAdding, setIsAdding] = createSignal(false);

  const addPrimitive = async (primitive: PrimitiveType) => {
    if (isAdding()) return;

    setIsAdding(true);
    setIsOpen(false);

    try {
      const name = `${primitive.type}_${Date.now()}`;
      // Use node graph based command - creates ActorNode with geometry network
      const actor = await invoke<Actor>('add_primitive_via_nodes', {
        name,
        primitive,
      });
      props.onObjectAdded(actor);
    } catch (error) {
      console.error('Failed to add primitive:', error);
    } finally {
      setIsAdding(false);
    }
  };

  const addEmptyObject = async () => {
    if (isAdding()) return;

    setIsAdding(true);
    setIsOpen(false);

    try {
      const name = `Empty_${Date.now()}`;
      // Use node graph command - creates ActorNode without geometry
      const actorId = await invoke<number>('create_node', {
        nodeType: 'actor',
        name,
      });

      // Create an Actor object for local state
      const actor: Actor = {
        id: actorId,
        name,
        transform: {
          position: { x: 0, y: 0, z: 0 },
          rotation: { w: 1, x: 0, y: 0, z: 0 },
          scale: { x: 1, y: 1, z: 1 },
        },
        children: [],
      };

      props.onObjectAdded(actor);
    } catch (error) {
      console.error('Failed to add empty object:', error);
    } finally {
      setIsAdding(false);
    }
  };

  return (
    <div class="add-object-menu">
      <button
        class="add-object-button"
        onClick={() => setIsOpen(!isOpen())}
        disabled={isAdding()}
      >
        {isAdding() ? 'Adding...' : '+ Add Object'}
      </button>

      <Show when={isOpen()}>
        <div class="add-object-dropdown">
          <button
            class="dropdown-item"
            onClick={() => addEmptyObject()}
          >
            <span class="item-icon">◯</span>
            Empty Object
          </button>
          <button
            class="dropdown-item"
            onClick={() => addPrimitive({ type: 'cube', size: 1.0 })}
          >
            <span class="item-icon">&#x25A0;</span>
            Cube
          </button>
          <button
            class="dropdown-item"
            onClick={() =>
              addPrimitive({ type: 'sphere', radius: 1.0, segments: 16, rings: 16 })
            }
          >
            <span class="item-icon">&#x25CF;</span>
            Sphere
          </button>
          <button
            class="dropdown-item"
            onClick={() =>
              addPrimitive({
                type: 'torus',
                major_radius: 1.0,
                minor_radius: 0.3,
                major_segments: 32,
                minor_segments: 16,
              })
            }
          >
            <span class="item-icon">&#x25EF;</span>
            Torus
          </button>
          <button
            class="dropdown-item"
            onClick={() => addPrimitive({ type: 'plane', width: 2.0, depth: 2.0 })}
          >
            <span class="item-icon">&#x25AD;</span>
            Plane
          </button>
          <button
            class="dropdown-item"
            onClick={() =>
              addPrimitive({ type: 'cylinder', radius: 0.5, height: 1.0, segments: 32 })
            }
          >
            <span class="item-icon">&#x25AF;</span>
            Cylinder
          </button>
          <button
            class="dropdown-item"
            onClick={() =>
              addPrimitive({ type: 'cone', radius: 0.5, height: 1.0, segments: 32 })
            }
          >
            <span class="item-icon">&#x25B2;</span>
            Cone
          </button>
        </div>
      </Show>
    </div>
  );
};
