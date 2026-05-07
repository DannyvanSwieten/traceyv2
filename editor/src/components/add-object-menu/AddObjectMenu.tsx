import { Component, createSignal, Show } from 'solid-js';
import * as api from '../../lib/api';
import './AddObjectMenu.css';

type Actor = api.Actor;

interface AddObjectMenuProps {
  onObjectAdded: (actor: Actor) => void;
}

export const AddObjectMenu: Component<AddObjectMenuProps> = (props) => {
  const [isOpen, setIsOpen] = createSignal(false);
  const [isAdding, setIsAdding] = createSignal(false);

  const addPrimitive = async (params: api.PrimitiveParams) => {
    if (isAdding()) return;

    setIsAdding(true);
    setIsOpen(false);

    try {
      const name = `${params.type}_${Date.now()}`;
      const actor = await api.addPrimitive(name, params);
      props.onObjectAdded(actor);
    } catch (error) {
      console.error('Failed to add primitive:', error);
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
