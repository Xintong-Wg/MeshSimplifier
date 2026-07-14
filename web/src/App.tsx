import { ChangeEvent, Suspense, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Canvas, ThreeEvent, useThree } from '@react-three/fiber';
import { Bounds, Center, Grid, Html, OrbitControls, useBounds, useGLTF } from '@react-three/drei';
import type { OrbitControls as OrbitControlsImpl } from 'three-stdlib';
import {
  Box,
  ChevronRight,
  CircleCheck,
  Eye,
  EyeOff,
  FileDown,
  FolderOpen,
  Gauge,
  GitMerge,
  Layers3,
  Loader2,
  Maximize2,
  MousePointer2,
  Pencil,
  Play,
  SlidersHorizontal,
  Sparkles,
  ScanLine,
  Split,
  Trash2,
} from 'lucide-react';
import * as THREE from 'three';

type ConvertMetadata = {
  partCount?: number;
  shapeCount?: number;
  verticesBefore?: number;
  trianglesBefore?: number;
  verticesAfter?: number;
  trianglesAfter?: number;
  reductionRatio?: number;
  hierarchy?: TreeNode;
  shapes?: Array<{
    key: string;
    verticesBefore: number;
    trianglesBefore: number;
    verticesAfter: number;
    trianglesAfter: number;
    achievedRatio: number;
  }>;
};

type ViewCommand = {
  token: number;
};

type CollapsibleSection = 'input' | 'parameters' | 'tree';

type TreeNode = {
  id?: string;
  name?: string;
  type?: string;
  shapeKey?: string;
  children?: TreeNode[];
};

type ConvertResponse = {
  ok: boolean;
  format?: 'glb' | 'stl';
  modelUrl?: string;
  fileUrl?: string;
  metadata?: ConvertMetadata;
  error?: string;
};

type ImportResponse = {
  ok: boolean;
  inputPath?: string;
  fileName?: string;
  size?: number;
  cacheDir?: string;
  modelUrl?: string;
  metadataUrl?: string;
  metadata?: ConvertMetadata;
  error?: string;
};

type ProcessLog = {
  id: number;
  text: string;
};

type PartExportFile = {
  name?: string;
  fileName?: string;
  url?: string;
  triangles?: number;
};

type PartExportResponse = {
  ok: boolean;
  folderPath?: string;
  zipUrl?: string;
  zipFileName?: string;
  files?: PartExportFile[];
  metadata?: ConvertMetadata & { files?: PartExportFile[] };
  error?: string;
};

type WritableFileStreamLike = {
  write: (data: Blob | BufferSource | string) => Promise<void>;
  close: () => Promise<void>;
};

type WritableFileHandleLike = {
  createWritable: () => Promise<WritableFileStreamLike>;
};

type WritableDirectoryHandleLike = {
  name: string;
  getDirectoryHandle: (name: string, options?: { create?: boolean }) => Promise<WritableDirectoryHandleLike>;
  getFileHandle: (name: string, options?: { create?: boolean }) => Promise<WritableFileHandleLike>;
};

type DirectoryPickerCapableWindow = Window & {
  showDirectoryPicker?: (options?: { id?: string; mode?: 'read' | 'readwrite' }) => Promise<WritableDirectoryHandleLike>;
};

type PartExportDialogState = {
  open: boolean;
  folderName: string;
  directory?: WritableDirectoryHandleLike;
  directoryName?: string;
  error?: string;
  mode?: 'batch' | 'single';
  sourceNodeId?: string;
  sourceNodeName?: string;
};

type TreeIndex = {
  nodeById: Map<string, TreeNode>;
  parentById: Map<string, string | undefined>;
  descendantsById: Map<string, Set<string>>;
  nameToIds: Map<string, string[]>;
  partIdsInOrder: string[];
  partIdsByShapeKey: Map<string, string[]>;
};

type SelectionRect = {
  left: number;
  top: number;
  width: number;
  height: number;
  token: number;
  additive: boolean;
};

type ViewportContextMenu = {
  id: string;
  x: number;
  y: number;
};

type TreeContextMenu = {
  id: string;
  x: number;
  y: number;
};

function nodeId(node: TreeNode) {
  return node.id || node.name || node.shapeKey || 'node';
}

function cloneTree(node?: TreeNode): TreeNode | undefined {
  if (!node) return undefined;
  return {
    ...node,
    children: node.children?.map((child) => cloneTree(child)!).filter(Boolean),
  };
}

function buildTreeIndex(root?: TreeNode): TreeIndex {
  const index: TreeIndex = {
    nodeById: new Map(),
    parentById: new Map(),
    descendantsById: new Map(),
    nameToIds: new Map(),
    partIdsInOrder: [],
    partIdsByShapeKey: new Map(),
  };

  function visit(node: TreeNode, parentId?: string): Set<string> {
    const id = nodeId(node);
    index.nodeById.set(id, node);
    index.parentById.set(id, parentId);
    if (node.type === 'part') {
      index.partIdsInOrder.push(id);
      if (node.shapeKey) {
        const partIds = index.partIdsByShapeKey.get(node.shapeKey) || [];
        partIds.push(id);
        index.partIdsByShapeKey.set(node.shapeKey, partIds);
      }
    }
    for (const key of [node.name, node.id, node.shapeKey].filter(Boolean) as string[]) {
      const ids = index.nameToIds.get(key) || [];
      ids.push(id);
      index.nameToIds.set(key, ids);
    }

    const descendants = new Set<string>([id]);
    for (const child of node.children || []) {
      for (const childId of visit(child, id)) descendants.add(childId);
    }
    index.descendantsById.set(id, descendants);
    return descendants;
  }

  if (root) visit(root);
  return index;
}

function selectedWithDescendants(selectedIds: string[], index: TreeIndex) {
  const ids = new Set<string>();
  selectedIds.forEach((id) => {
    const descendants = index.descendantsById.get(id);
    if (descendants) descendants.forEach((childId) => ids.add(childId));
    else ids.add(id);
  });
  return ids;
}

function uniqueVisibleCandidate(candidates: string[], index: TreeIndex, hiddenIds?: Set<string>) {
  const visible = candidates.filter((id) => index.nodeById.has(id) && !hiddenIds?.has(id));
  const source = visible.length ? visible : candidates.filter((id) => index.nodeById.has(id));
  return source.length === 1 ? source[0] : undefined;
}

function findNodeIdsByObjectName(name: string, index: TreeIndex, hiddenIds?: Set<string>) {
  const ids = new Set<string>();
  const exactNode = index.nodeById.get(name);
  if (exactNode && !hiddenIds?.has(name)) ids.add(name);

  const exactCandidates = index.nameToIds.get(name) || [];
  exactCandidates.forEach((id) => {
    const node = index.nodeById.get(id);
    if (!node || hiddenIds?.has(id)) return;
    if (node.id === name) ids.add(id);
  });

  if (!ids.size) {
    const nameCandidates = exactCandidates.filter((id) => {
      const node = index.nodeById.get(id);
      return node?.name === name;
    });
    const uniqueNameMatch = uniqueVisibleCandidate(nameCandidates, index, hiddenIds);
    if (uniqueNameMatch) ids.add(uniqueNameMatch);
  }

  return [...ids];
}

function removeNodesFromTree(root: TreeNode | undefined, ids: Set<string>): TreeNode | undefined {
  if (!root) return undefined;
  if (ids.has(nodeId(root))) return undefined;
  return {
    ...root,
    children: (root.children || [])
      .map((child) => removeNodesFromTree(child, ids))
      .filter(Boolean) as TreeNode[],
  };
}

function renameNodeInTree(root: TreeNode | undefined, id: string, name: string): TreeNode | undefined {
  if (!root) return undefined;
  const currentId = nodeId(root);
  if (currentId === id) {
    return { ...root, id: root.id || currentId, name };
  }
  return {
    ...root,
    children: root.children?.map((child) => renameNodeInTree(child, id, name)!).filter(Boolean),
  };
}

function normalizeMergeIds(ids: string[], index: TreeIndex) {
  const selected = new Set(ids);
  return ids.filter((id) => {
    let parent = index.parentById.get(id);
    while (parent) {
      if (selected.has(parent)) return false;
      parent = index.parentById.get(parent);
    }
    return true;
  });
}

function mergeNodesInTree(root: TreeNode | undefined, ids: string[], index: TreeIndex, groupId: string): TreeNode | undefined {
  if (!root || ids.length < 2) return root;
  const normalized = normalizeMergeIds(ids, index);
  const selected = new Set(normalized);
  const selectedNodes = normalized
    .map((id) => cloneTree(index.nodeById.get(id)))
    .filter(Boolean) as TreeNode[];
  if (selectedNodes.length < 2) return root;

  const firstParentId = index.parentById.get(normalized[0]);
  function visit(node: TreeNode): TreeNode | undefined {
    const id = nodeId(node);
    if (selected.has(id)) return undefined;

    const nextChildren: TreeNode[] = [];
    for (const child of node.children || []) {
      const next = visit(child);
      if (next) nextChildren.push(next);
    }

    if (id === firstParentId) {
      nextChildren.push({
        id: groupId,
        name: `合并组 ${selectedNodes.length} 项`,
        type: 'assembly',
        children: selectedNodes,
      });
    }

    return { ...node, children: nextChildren };
  }

  const merged = visit(root);
  if (merged && !firstParentId) {
    merged.children = [
      ...(merged.children || []),
      { id: groupId, name: `合并组 ${selectedNodes.length} 项`, type: 'assembly', children: selectedNodes },
    ];
  }
  return merged;
}

function splitNodeInTree(root: TreeNode | undefined, idToSplit: string): TreeNode | undefined {
  if (!root) return undefined;
  const id = nodeId(root);
  if (id === idToSplit) return root;

  const children: TreeNode[] = [];
  for (const child of root.children || []) {
    const childId = nodeId(child);
    if (childId === idToSplit) {
      children.push(...(child.children || []).map((item) => cloneTree(item)!).filter(Boolean));
      continue;
    }
    const next = splitNodeInTree(child, idToSplit);
    if (next) children.push(next);
  }

  return { ...root, children };
}

function formatNumber(value?: number) {
  if (typeof value !== 'number' || Number.isNaN(value)) return '--';
  return new Intl.NumberFormat('zh-CN').format(Math.round(value));
}

function formatPercent(value?: number) {
  if (typeof value !== 'number' || Number.isNaN(value)) return '--';
  return `${Math.round(value * 1000) / 10}%`;
}

function formatBytes(value?: number) {
  if (typeof value !== 'number' || Number.isNaN(value)) return '--';
  if (value >= 1024 ** 3) return `${(value / 1024 ** 3).toFixed(2)} GB`;
  if (value >= 1024 ** 2) return `${(value / 1024 ** 2).toFixed(1)} MB`;
  if (value >= 1024) return `${(value / 1024).toFixed(1)} KB`;
  return `${value} B`;
}

function stripCadExtension(name: string) {
  return name.replace(/\.(step|stp|iges|igs)$/i, '');
}

function sanitizeWindowsName(name: string, fallback: string) {
  const normalized = name
    .replace(/[<>:"/\\|?*\x00-\x1F]/g, '_')
    .replace(/\s+/g, ' ')
    .replace(/[. ]+$/g, '')
    .trim();
  return normalized.slice(0, 160) || fallback;
}

function triggerBrowserDownload(url: string, fileName: string) {
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = fileName;
  anchor.rel = 'noopener';
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
}

function defaultPartExportFolderName(importedName: string, inputPath: string) {
  const rawName = importedName || inputPath.split(/[\\/]/).pop() || 'model';
  return `${sanitizeWindowsName(stripCadExtension(rawName), 'model')}-parts`;
}

function assignedNodeIdsFromObject(object: THREE.Object3D) {
  let current: THREE.Object3D | null = object;
  while (current) {
    const ids = current.userData?.meshForgeNodeIds;
    if (Array.isArray(ids) && ids.every((id) => typeof id === 'string')) return ids as string[];
    current = current.parent;
  }
  return [];
}

function modelNodeIdFromObject(object: THREE.Object3D, index: TreeIndex, hiddenIds: Set<string>) {
  return modelNodeIdsFromObject(object, index, hiddenIds)[0];
}

function modelNodeIdsFromObjectNameChain(object: THREE.Object3D, index: TreeIndex, hiddenIds?: Set<string>) {
  const structuralMatches: string[] = [];
  const seenStructural = new Set<string>();
  let current: THREE.Object3D | null = object;

  while (current) {
    const currentName = current.name;
    if (currentName) {
      findNodeIdsByObjectName(currentName, index, hiddenIds).forEach((id) => {
        if (!seenStructural.has(id)) {
          seenStructural.add(id);
          structuralMatches.push(id);
        }
      });
    }
    current = current.parent;
  }

  const leafPart = structuralMatches.find((id) => {
    const node = index.nodeById.get(id);
    return node?.type === 'part' || !(node?.children?.length);
  });
  if (leafPart) return [leafPart];

  const leafStructural = structuralMatches.find((id) => !(index.nodeById.get(id)?.children?.length));
  if (leafStructural) return [leafStructural];

  return structuralMatches.length ? [structuralMatches[0]] : [];
}

function modelNodeIdsFromObject(object: THREE.Object3D, index: TreeIndex, hiddenIds: Set<string>) {
  const assigned = assignedNodeIdsFromObject(object);
  if (assigned.length) return assigned.filter((id) => !hiddenIds.has(id));
  return modelNodeIdsFromObjectNameChain(object, index, hiddenIds);
}

function modelNodeIdsForHighlight(object: THREE.Object3D, index: TreeIndex) {
  const assigned = assignedNodeIdsFromObject(object);
  if (assigned.length) return assigned;
  const partIds = modelNodeIdsFromObjectNameChain(object, index);
  if (partIds.length) return partIds;
  const ids: string[] = [];
  const seen = new Set<string>();
  let current: THREE.Object3D | null = object;
  while (current) {
    const currentName = current.name;
    if (currentName) {
      findNodeIdsByObjectName(currentName, index).forEach((id) => {
        if (!seen.has(id)) {
          seen.add(id);
          ids.push(id);
        }
      });
    }
    current = current.parent;
  }
  return ids;
}

function objectMatchesHiddenKey(object: THREE.Object3D, hiddenKeys: Set<string>) {
  let current: THREE.Object3D | null = object;
  while (current) {
    if (current.name && hiddenKeys.has(current.name)) return true;
    current = current.parent;
  }
  return false;
}

function collectNodeKeys(ids: Set<string>, index: TreeIndex) {
  const keys = new Set<string>();
  ids.forEach((id) => {
    const node = index.nodeById.get(id);
    [id, node?.id, node?.name, node?.shapeKey].forEach((key) => {
      if (key) keys.add(key);
    });
  });
  return keys;
}

function collectNodeExclusionTokens(ids: Set<string>, index: TreeIndex) {
  const tokens = new Set<string>();
  ids.forEach((id) => {
    const node = index.nodeById.get(id);
    [id, node?.id].forEach((value) => {
      if (value) {
        tokens.add(value);
        tokens.add(`id:${value}`);
      }
    });
  });
  return tokens;
}

function collectExclusionTokensExcept(keepIds: Set<string>, index: TreeIndex, baseTokens?: Set<string>) {
  const preservedIds = new Set<string>(keepIds);
  keepIds.forEach((id) => {
    let parent = index.parentById.get(id);
    while (parent) {
      preservedIds.add(parent);
      parent = index.parentById.get(parent);
    }
  });

  const idsToExclude = new Set<string>();
  index.nodeById.forEach((_node, id) => {
    if (!preservedIds.has(id)) idsToExclude.add(id);
  });
  return unionSets(baseTokens || new Set<string>(), collectNodeExclusionTokens(idsToExclude, index));
}

function collectNodeIdExclusionTokensExcept(keepIds: Set<string>, index: TreeIndex, baseTokens?: Set<string>) {
  const preservedIds = new Set<string>(keepIds);
  keepIds.forEach((id) => {
    let parent = index.parentById.get(id);
    while (parent) {
      preservedIds.add(parent);
      parent = index.parentById.get(parent);
    }
  });

  const tokens = new Set<string>(
    [...(baseTokens || new Set<string>())].filter((token) => !token.startsWith('name:') && !token.startsWith('shape:')),
  );
  index.nodeById.forEach((_node, id) => {
    if (!preservedIds.has(id)) {
      tokens.add(id);
      tokens.add(`id:${id}`);
    }
  });
  return tokens;
}

function collectNodeIdIncludeTokens(ids: Set<string>, index: TreeIndex) {
  const tokens = new Set<string>();
  ids.forEach((id) => {
    const node = index.nodeById.get(id);
    if (!node) return;
    [id, node.id].forEach((value) => {
      if (value) {
        tokens.add(value);
        tokens.add(`id:${value}`);
      }
    });
  });
  return tokens;
}

function unionSets<T>(...sets: Set<T>[]) {
  const result = new Set<T>();
  sets.forEach((set) => set.forEach((value) => result.add(value)));
  return result;
}

function assignModelNodeIds(model: THREE.Object3D, index: TreeIndex) {
  const assigned = new Set<string>();
  const shapeCursors = new Map<string, number>();
  let partCursor = 0;

  function markAssigned(ids: string[]) {
    ids.forEach((id) => assigned.add(id));
  }

  function nextPartForShape(shapeKey?: string) {
    if (!shapeKey) return undefined;
    const partIds = index.partIdsByShapeKey.get(shapeKey);
    if (!partIds?.length) return undefined;
    let cursor = shapeCursors.get(shapeKey) || 0;
    while (cursor < partIds.length && assigned.has(partIds[cursor])) cursor += 1;
    shapeCursors.set(shapeKey, cursor + 1);
    return partIds[cursor];
  }

  function nextPartInOrder() {
    while (partCursor < index.partIdsInOrder.length && assigned.has(index.partIdsInOrder[partCursor])) {
      partCursor += 1;
    }
    return index.partIdsInOrder[partCursor++];
  }

  model.traverse((object) => {
    if (!(object as THREE.Mesh).isMesh) return;
    const mesh = object as THREE.Mesh;
    let ids = modelNodeIdsFromObjectNameChain(mesh, index);

    if (!ids.length) {
      const shapeCandidates = [
        mesh.geometry?.name,
        Array.isArray(mesh.material) ? undefined : mesh.material?.name,
      ].filter(Boolean) as string[];
      for (const shapeKey of shapeCandidates) {
        const partId = nextPartForShape(shapeKey);
        if (partId) {
          ids = [partId];
          break;
        }
      }
    }

    if (!ids.length) {
      const partId = nextPartInOrder();
      if (partId) ids = [partId];
    }

    mesh.userData.meshForgeNodeIds = ids;
    markAssigned(ids);
  });
}

function ModelScene({
  url,
  wireframe,
  treeIndex,
  modelTreeIndex,
  highlightedIds,
  hiddenIds,
  hiddenObjectKeys,
  onPick,
  onContextPick,
  selectionRect,
  onBoxSelect,
}: {
  url?: string;
  wireframe: boolean;
  treeIndex: TreeIndex;
  modelTreeIndex: TreeIndex;
  highlightedIds: Set<string>;
  hiddenIds: Set<string>;
  hiddenObjectKeys: Set<string>;
  onPick: (id: string, additive: boolean) => void;
  onContextPick: (id: string, clientX: number, clientY: number) => void;
  selectionRect?: SelectionRect;
  onBoxSelect: (ids: string[], additive: boolean) => void;
}) {
  if (!url) {
    return null;
  }

  return (
    <LoadedModel
      url={url}
      wireframe={wireframe}
      treeIndex={treeIndex}
      modelTreeIndex={modelTreeIndex}
      highlightedIds={highlightedIds}
      hiddenIds={hiddenIds}
      hiddenObjectKeys={hiddenObjectKeys}
      onPick={onPick}
      onContextPick={onContextPick}
      selectionRect={selectionRect}
      onBoxSelect={onBoxSelect}
    />
  );
}

function LoadedModel({
  url,
  wireframe,
  treeIndex,
  modelTreeIndex,
  highlightedIds,
  hiddenIds,
  hiddenObjectKeys,
  onPick,
  onContextPick,
  selectionRect,
  onBoxSelect,
}: {
  url: string;
  wireframe: boolean;
  treeIndex: TreeIndex;
  modelTreeIndex: TreeIndex;
  highlightedIds: Set<string>;
  hiddenIds: Set<string>;
  hiddenObjectKeys: Set<string>;
  onPick: (id: string, additive: boolean) => void;
  onContextPick: (id: string, clientX: number, clientY: number) => void;
  selectionRect?: SelectionRect;
  onBoxSelect: (ids: string[], additive: boolean) => void;
}) {
  const { scene } = useGLTF(url);
  const { camera, size } = useThree();
  const model = useMemo(() => {
    const cloned = scene.clone(true);
    assignModelNodeIds(cloned, modelTreeIndex);
    cloned.traverse((object) => {
      if ((object as THREE.Mesh).isMesh) {
        const mesh = object as THREE.Mesh;
        mesh.castShadow = true;
        mesh.receiveShadow = true;
        const source = Array.isArray(mesh.material) ? mesh.material[0] : mesh.material;
        const material = source instanceof THREE.MeshStandardMaterial
          ? source.clone()
          : new THREE.MeshStandardMaterial({ color: '#cbd5e1' });
        material.wireframe = wireframe;
        material.roughness = 0.52;
        material.metalness = 0.18;
        mesh.material = material;
      }
    });
    return cloned;
  }, [modelTreeIndex, scene, wireframe]);

  useEffect(() => {
    model.traverse((object) => {
      const ids = modelNodeIdsForHighlight(object, modelTreeIndex);
      const hidden = objectMatchesHiddenKey(object, hiddenObjectKeys) || ids.some((id) => hiddenIds.has(id));
      object.visible = !hidden;

      if ((object as THREE.Mesh).isMesh) {
        const mesh = object as THREE.Mesh;
        const materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
        const highlighted = ids.some((id) => highlightedIds.has(id));
        materials.forEach((material) => {
          if (material instanceof THREE.MeshStandardMaterial) {
            material.color.set(highlighted ? '#ff7a1a' : '#cbd5e1');
            material.emissive.set(highlighted ? '#ff6b00' : '#000000');
            material.emissiveIntensity = highlighted ? 0.62 : 0;
            material.opacity = highlighted ? 1 : 0.92;
            material.transparent = true;
            material.needsUpdate = true;
          }
        });
      }
    });
  }, [model, modelTreeIndex, highlightedIds, hiddenIds, hiddenObjectKeys]);

  useEffect(() => {
    if (!selectionRect) return;
    const ids = new Set<string>();
    model.traverse((object) => {
      if (!(object as THREE.Mesh).isMesh || !object.visible) return;
      const id = modelNodeIdFromObject(object, modelTreeIndex, hiddenIds);
      if (!id) return;
      const box = new THREE.Box3().setFromObject(object);
      if (box.isEmpty()) return;
      const { min, max } = box;
      const points = [
        box.getCenter(new THREE.Vector3()),
        new THREE.Vector3(min.x, min.y, min.z),
        new THREE.Vector3(min.x, min.y, max.z),
        new THREE.Vector3(min.x, max.y, min.z),
        new THREE.Vector3(min.x, max.y, max.z),
        new THREE.Vector3(max.x, min.y, min.z),
        new THREE.Vector3(max.x, min.y, max.z),
        new THREE.Vector3(max.x, max.y, min.z),
        new THREE.Vector3(max.x, max.y, max.z),
      ];
      const intersectsRect = points.some((point) => {
        const projected = point.project(camera);
        const x = (projected.x * 0.5 + 0.5) * size.width;
        const y = (-projected.y * 0.5 + 0.5) * size.height;
        return (
          x >= selectionRect.left &&
          x <= selectionRect.left + selectionRect.width &&
          y >= selectionRect.top &&
          y <= selectionRect.top + selectionRect.height
        );
      });
      if (intersectsRect) {
        ids.add(id);
      }
    });
    onBoxSelect([...ids], selectionRect.additive);
  }, [camera, hiddenIds, model, modelTreeIndex, onBoxSelect, selectionRect, size.height, size.width]);

  function handleClick(event: ThreeEvent<MouseEvent>) {
    event.stopPropagation();
    const id = modelNodeIdFromObject(event.object, modelTreeIndex, hiddenIds);
    if (id) onPick(id, event.shiftKey || event.ctrlKey || event.metaKey);
  }

  function handleContextMenu(event: ThreeEvent<MouseEvent>) {
    event.nativeEvent.preventDefault();
    const id = modelNodeIdFromObject(event.object, modelTreeIndex, hiddenIds);
    if (id) onContextPick(id, event.nativeEvent.clientX, event.nativeEvent.clientY);
  }

  return <primitive object={model} onClick={handleClick} onContextMenu={handleContextMenu} />;
}

function ViewCommandHandler({
  command,
  controlsRef,
}: {
  command?: ViewCommand;
  controlsRef: React.RefObject<OrbitControlsImpl>;
}) {
  const bounds = useBounds();
  const { camera } = useThree();
  const lastTokenRef = useRef(0);

  useEffect(() => {
    if (!command || command.token === lastTokenRef.current) return;
    lastTokenRef.current = command.token;

    bounds.refresh().fit().clip();
    const controls = controlsRef.current;
    if (controls) {
      const { center } = bounds.getSize();
      controls.target.copy(center);
      controls.update();
      controls.saveState();
    }
  }, [bounds, camera, command, controlsRef]);

  return null;
}

function SceneViewport({
  modelUrl,
  wireframe,
  viewCommand,
  treeIndex,
  modelTreeIndex,
  selectedNodeIds,
  hiddenNodeIds,
  hiddenObjectKeys,
  onSelectNode,
  onBoxSelect,
  onRenameNode,
  onDeleteSelected,
  onMergeSelected,
  onSplitNode,
  onExportNode,
  onHideNode,
  onShowNode,
  onIsolateNode,
  canMergeSelection,
  onClearSelection,
}: {
  modelUrl?: string;
  wireframe: boolean;
  viewCommand?: ViewCommand;
  treeIndex: TreeIndex;
  modelTreeIndex: TreeIndex;
  selectedNodeIds: string[];
  hiddenNodeIds: Set<string>;
  hiddenObjectKeys: Set<string>;
  onSelectNode: (id: string, additive?: boolean) => void;
  onBoxSelect: (ids: string[], additive?: boolean) => void;
  onRenameNode: (id: string, name: string) => void;
  onDeleteSelected: () => void;
  onMergeSelected: () => void;
  onSplitNode: (id: string) => void;
  onExportNode: (id: string) => void;
  onHideNode: (id: string) => void;
  onShowNode: (id: string) => void;
  onIsolateNode: (id: string) => void;
  canMergeSelection: boolean;
  onClearSelection: () => void;
}) {
  const shellRef = useRef<HTMLDivElement>(null);
  const controlsRef = useRef<OrbitControlsImpl>(null);
  const dragStartRef = useRef<{ x: number; y: number; additive: boolean; moved: boolean }>();
  const rightDragRef = useRef<{ x: number; y: number; moved: boolean }>();
  const suppressContextMenuUntilRef = useRef(0);
  const [dragRect, setDragRect] = useState<SelectionRect>();
  const [selectionRect, setSelectionRect] = useState<SelectionRect>();
  const [contextMenu, setContextMenu] = useState<ViewportContextMenu>();
  const highlightedIds = useMemo(() => selectedWithDescendants(selectedNodeIds, treeIndex), [selectedNodeIds, treeIndex]);
  const contextNode = contextMenu ? treeIndex.nodeById.get(contextMenu.id) : undefined;

  function onPointerDown(event: React.PointerEvent<HTMLDivElement>) {
    if (event.button !== 0) return;
    const target = event.target as HTMLElement;
    if (target.closest('.selection-box') || target.closest('.viewport-context-menu') || target.closest('.viewport-badges')) return;
    const bounds = shellRef.current?.getBoundingClientRect();
    if (!bounds) return;
    setContextMenu(undefined);
    dragStartRef.current = {
      x: event.clientX - bounds.left,
      y: event.clientY - bounds.top,
      additive: event.shiftKey || event.ctrlKey || event.metaKey,
      moved: false,
    };
  }

  function onPointerDownCapture(event: React.PointerEvent<HTMLDivElement>) {
    if (event.button !== 2) return;
    rightDragRef.current = { x: event.clientX, y: event.clientY, moved: false };
  }

  function onPointerMove(event: React.PointerEvent<HTMLDivElement>) {
    if (rightDragRef.current) {
      const dx = event.clientX - rightDragRef.current.x;
      const dy = event.clientY - rightDragRef.current.y;
      if (Math.hypot(dx, dy) > 5) rightDragRef.current.moved = true;
    }

    const start = dragStartRef.current;
    const bounds = shellRef.current?.getBoundingClientRect();
    if (!start || !bounds) return;
    const x = event.clientX - bounds.left;
    const y = event.clientY - bounds.top;
    const width = Math.abs(x - start.x);
    const height = Math.abs(y - start.y);
    if (width < 8 && height < 8) return;
    start.moved = true;
    setDragRect({
      left: Math.min(start.x, x),
      top: Math.min(start.y, y),
      width,
      height,
      token: 0,
      additive: start.additive,
    });
  }

  function onPointerUp(event: React.PointerEvent<HTMLDivElement>) {
    if (event.button === 2 && rightDragRef.current?.moved) {
      suppressContextMenuUntilRef.current = Date.now() + 250;
    }
    if (event.button === 2) {
      rightDragRef.current = undefined;
      return;
    }

    const start = dragStartRef.current;
    if (dragRect && start) {
      const finalRect = { ...dragRect, token: Date.now(), additive: start.additive };
      setSelectionRect(finalRect);
      window.setTimeout(() => setSelectionRect(undefined), 80);
    }
    setDragRect(undefined);
    dragStartRef.current = undefined;
  }

  function openContextMenu(id: string, clientX: number, clientY: number) {
    if (Date.now() < suppressContextMenuUntilRef.current || rightDragRef.current?.moved) return;
    const bounds = shellRef.current?.getBoundingClientRect();
    if (!bounds) return;
    if (!highlightedIds.has(id)) onSelectNode(id, false);
    setContextMenu({
      id,
      x: Math.min(Math.max(clientX - bounds.left, 8), bounds.width - 164),
      y: Math.min(Math.max(clientY - bounds.top, 8), bounds.height - 132),
    });
  }

  function renameFromMenu() {
    if (!contextMenu) return;
    const currentName = contextNode?.name || contextNode?.id || contextMenu.id;
    const nextName = window.prompt('重命名装配/零件', currentName);
    if (nextName?.trim()) onRenameNode(contextMenu.id, nextName.trim());
    setContextMenu(undefined);
  }

  return (
    <div
      ref={shellRef}
      className="viewport-shell"
      onContextMenu={(event) => event.preventDefault()}
      onPointerDownCapture={onPointerDownCapture}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerLeave={onPointerUp}
    >
      {dragRect && <div className="selection-box" style={{ left: dragRect.left, top: dragRect.top, width: dragRect.width, height: dragRect.height }} />}
      {contextMenu && (
        <div
          className="viewport-context-menu"
          style={{ left: contextMenu.x, top: contextMenu.y }}
          onPointerDown={(event) => event.stopPropagation()}
        >
          <button onClick={renameFromMenu}>
            <Pencil size={13} />
            重命名
          </button>
          <button
            disabled={!contextNode || contextNode.type === 'root'}
            onClick={() => {
              if (!contextMenu) return;
              onExportNode(contextMenu.id);
              setContextMenu(undefined);
            }}
          >
            <FileDown size={13} />
            单独导出
          </button>
          <button
            onClick={() => {
              if (!contextMenu) return;
              if (hiddenNodeIds.has(contextMenu.id)) onShowNode(contextMenu.id);
              else onHideNode(contextMenu.id);
              setContextMenu(undefined);
            }}
          >
            {contextMenu && hiddenNodeIds.has(contextMenu.id) ? <Eye size={13} /> : <EyeOff size={13} />}
            {contextMenu && hiddenNodeIds.has(contextMenu.id) ? '显示' : '隐藏'}
          </button>
          <button
            onClick={() => {
              if (!contextMenu) return;
              onIsolateNode(contextMenu.id);
              setContextMenu(undefined);
            }}
          >
            <Eye size={13} />
            仅显
          </button>
          <button
            onClick={() => {
              onDeleteSelected();
              setContextMenu(undefined);
            }}
          >
            <Trash2 size={13} />
            删除
          </button>
          <button
            disabled={!canMergeSelection}
            onClick={() => {
              onMergeSelected();
              setContextMenu(undefined);
            }}
          >
            <GitMerge size={13} />
            合并
          </button>
          <button
            disabled={!contextNode?.children?.length || contextNode.type === 'root'}
            onClick={() => {
              if (!contextMenu) return;
              onSplitNode(contextMenu.id);
              setContextMenu(undefined);
            }}
          >
            <Split size={13} />
            拆分
          </button>
        </div>
      )}
      <Canvas
        camera={{ position: [4, 3.2, 5], fov: 44 }}
        onPointerMissed={(event) => {
          if (event.type !== 'click' || event.button !== 0) return;
          if (dragStartRef.current?.moved) return;
          setContextMenu(undefined);
          onClearSelection();
        }}
      >
        <color attach="background" args={['#eef1f5']} />
        <ambientLight intensity={0.8} />
        <directionalLight position={[4, 6, 5]} intensity={1.4} />
        <Suspense fallback={<Html center><div className="scene-loading">加载模型...</div></Html>}>
          {modelUrl ? (
            <Bounds fit clip observe margin={1.25}>
              <ViewCommandHandler command={viewCommand} controlsRef={controlsRef} />
              <Center>
                <ModelScene
                  url={modelUrl}
                  wireframe={wireframe}
                  treeIndex={treeIndex}
                  modelTreeIndex={modelTreeIndex}
                  highlightedIds={highlightedIds}
                  hiddenIds={hiddenNodeIds}
                  hiddenObjectKeys={hiddenObjectKeys}
                  onPick={onSelectNode}
                  onContextPick={openContextMenu}
                  selectionRect={selectionRect}
                  onBoxSelect={(ids, additive) => onBoxSelect(ids, additive)}
                />
              </Center>
            </Bounds>
          ) : (
            <Html center>
              <div className="scene-empty">
                <strong>等待导入模型</strong>
                <span>导入完成后会自动显示原模型，随后可继续简化。</span>
              </div>
            </Html>
          )}
        </Suspense>
        <Grid
          args={[16, 16]}
          cellSize={0.5}
          cellThickness={0.6}
          cellColor="#cbd5e1"
          sectionColor="#94a3b8"
          sectionThickness={1}
          fadeDistance={18}
          infiniteGrid
          position={[0, -1.12, 0]}
        />
        <OrbitControls
          ref={controlsRef}
          makeDefault
          enableDamping
          dampingFactor={0.08}
          enablePan
          enableZoom
          mouseButtons={{ LEFT: undefined, MIDDLE: THREE.MOUSE.PAN, RIGHT: THREE.MOUSE.ROTATE }}
        />
      </Canvas>
    </div>
  );
}

function TreeView({
  node,
  depth = 0,
  selectedIds,
  hiddenIds,
  onSelect,
  onContextMenu,
  onHide,
  onShow,
  onIsolate,
}: {
  node: TreeNode;
  depth?: number;
  selectedIds: Set<string>;
  hiddenIds: Set<string>;
  onSelect: (id: string, additive: boolean) => void;
  onContextMenu: (id: string, event: React.MouseEvent<HTMLElement>) => void;
  onHide: (id: string) => void;
  onShow: (id: string) => void;
  onIsolate: (id: string) => void;
}) {
  const children = node.children ?? [];
  const id = nodeId(node);
  const selected = selectedIds.has(id);
  const hidden = hiddenIds.has(id);
  const [expanded, setExpanded] = useState(true);
  return (
    <div className="tree-node" style={{ '--depth': depth } as React.CSSProperties}>
      <div className={selected ? 'tree-row selected' : hidden ? 'tree-row muted' : 'tree-row'}>
        {children.length > 0 ? (
          <button
            className="tree-toggle"
            title={expanded ? '收起' : '展开'}
            aria-label={expanded ? '收起节点' : '展开节点'}
            aria-expanded={expanded}
            onClick={(event) => {
              event.stopPropagation();
              setExpanded((value) => !value);
            }}
          >
            <ChevronRight className={expanded ? 'expanded' : ''} size={14} />
          </button>
        ) : (
          <span className="tree-spacer" />
        )}
        <button
          className="tree-row-main"
          data-node-id={id}
          aria-selected={selected}
          onClick={(event) => onSelect(id, event.shiftKey || event.ctrlKey || event.metaKey)}
          onContextMenu={(event) => onContextMenu(id, event)}
        >
          <Box size={14} />
          <span>{node.name || node.id || '未命名节点'}</span>
          {node.type && <em>{node.type}</em>}
        </button>
        <div className="tree-row-controls">
          <button
            className="tree-icon-action"
            title={hidden ? '显示' : '隐藏'}
            aria-label={hidden ? '显示' : '隐藏'}
            onClick={(event) => {
              event.stopPropagation();
              if (hidden) onShow(id);
              else onHide(id);
            }}
          >
            {hidden ? <Eye size={13} /> : <EyeOff size={13} />}
          </button>
          <button
            className="tree-text-action"
            title="仅显"
            aria-label="仅显"
            onClick={(event) => {
              event.stopPropagation();
              onIsolate(id);
            }}
          >
            仅显
          </button>
        </div>
      </div>
      {expanded && children.map((child, index) => (
        <TreeView
          key={`${child.id || child.name}-${index}`}
          node={child}
          depth={depth + 1}
          selectedIds={selectedIds}
          hiddenIds={hiddenIds}
          onSelect={onSelect}
          onContextMenu={onContextMenu}
          onHide={onHide}
          onShow={onShow}
          onIsolate={onIsolate}
        />
      ))}
    </div>
  );
}

function hasTreeContent(node?: TreeNode): node is TreeNode {
  if (!node) return false;
  if ((node.children?.length ?? 0) > 0) return true;
  return Boolean(node.name || node.id || node.type || node.shapeKey);
}

export function App() {
  const fileInputRef = useRef<HTMLInputElement>(null);
  const restoredFromUrlRef = useRef(false);
  const logIdRef = useRef(0);
  const [inputPath, setInputPath] = useState('');
  const [importedName, setImportedName] = useState('');
  const [importedSize, setImportedSize] = useState<number>();
  const [cacheDir, setCacheDir] = useState('');
  const [ratio, setRatio] = useState(0.35);
  const [linearDeflection, setLinearDeflection] = useState(0.5);
  const [targetError, setTargetError] = useState(0.05);
  const [wireframe, setWireframe] = useState(false);
  const [viewCommand, setViewCommand] = useState<ViewCommand>();
  const [modelUrl, setModelUrl] = useState<string>();
  const [exportFormat, setExportFormat] = useState<'glb' | 'stl'>('glb');
  const [exportUrl, setExportUrl] = useState<string>();
  const [exportName, setExportName] = useState('mesh-simplified.glb');
  const [partExports, setPartExports] = useState<PartExportFile[]>([]);
  const [metadata, setMetadata] = useState<ConvertMetadata>();
  const [modelHierarchy, setModelHierarchy] = useState<TreeNode>();
  const [status, setStatus] = useState('准备导入模型');
  const [logs, setLogs] = useState<ProcessLog[]>([{ id: 0, text: '等待导入 STEP / IGES 模型' }]);
  const [busy, setBusy] = useState(false);
  const [importing, setImporting] = useState(false);
  const [selectedNodeIds, setSelectedNodeIds] = useState<string[]>([]);
  const [hiddenNodeIds, setHiddenNodeIds] = useState<Set<string>>(new Set());
  const [hiddenObjectKeys, setHiddenObjectKeys] = useState<Set<string>>(new Set());
  const [deletedNodeIds, setDeletedNodeIds] = useState<Set<string>>(new Set());
  const [deletedObjectKeys, setDeletedObjectKeys] = useState<Set<string>>(new Set());
  const [deletedNodeExclusionTokens, setDeletedNodeExclusionTokens] = useState<Set<string>>(new Set());
  const [mergeCounter, setMergeCounter] = useState(0);
  const [treeContextMenu, setTreeContextMenu] = useState<TreeContextMenu>();
  const [collapsedSections, setCollapsedSections] = useState<Record<CollapsibleSection, boolean>>({
    input: false,
    parameters: false,
    tree: false,
  });
  const [partExportDialog, setPartExportDialog] = useState<PartExportDialogState>({
    open: false,
    folderName: 'model-parts',
    mode: 'batch',
  });

  function appendLog(message: string) {
    const time = new Date().toLocaleTimeString('zh-CN', { hour12: false });
    const entry = { id: ++logIdRef.current, text: `${time} ${message}` };
    setLogs((items) => [entry, ...items].slice(0, 12));
  }

  const toggleSection = useCallback((section: CollapsibleSection) => {
    setCollapsedSections((current) => ({ ...current, [section]: !current[section] }));
  }, []);

  const defaultExportFolderName = useMemo(
    () => defaultPartExportFolderName(importedName, inputPath),
    [importedName, inputPath],
  );
  const canPickExportDirectory = window.isSecureContext
    && typeof (window as DirectoryPickerCapableWindow).showDirectoryPicker === 'function';

  useEffect(() => {
    if (restoredFromUrlRef.current) return;
    const params = new URLSearchParams(window.location.search);
    const model = params.get('model');
    const metadataUrl = params.get('metadata');
    if (!model) return;
    restoredFromUrlRef.current = true;

    setModelUrl(`${model}${model.includes('?') ? '&' : '?'}t=${Date.now()}`);
    setExportUrl(model);
    setExportName('mesh-preview.glb');
    setSelectedNodeIds([]);
    setHiddenNodeIds(new Set());
    setHiddenObjectKeys(new Set());
    setDeletedNodeIds(new Set());
    setDeletedObjectKeys(new Set());
    setDeletedNodeExclusionTokens(new Set());
    setStatus('已加载导出模型预览');
    appendLog('通过导出结果链接加载 GLB 预览');

    if (!metadataUrl) return;
    fetch(metadataUrl)
      .then((response) => {
        if (!response.ok) throw new Error(`metadata HTTP ${response.status}`);
        return response.json() as Promise<ConvertMetadata>;
      })
      .then((data) => {
        setMetadata(data);
        setModelHierarchy(data.hierarchy);
        appendLog(`读取导出统计：${formatNumber(data.partCount)} 部件，${formatNumber(data.shapeCount)} 形体`);
      })
      .catch((error) => {
        appendLog(`导出统计读取失败：${error instanceof Error ? error.message : String(error)}`);
      });
  }, []);

  function uploadCadFile(file: File) {
    return new Promise<ImportResponse>((resolve, reject) => {
      const request = new XMLHttpRequest();
      let processingSeconds = 0;
      let processingLogTimer: number | undefined;
      const clearProcessingLogTimer = () => {
        if (processingLogTimer !== undefined) {
          window.clearInterval(processingLogTimer);
          processingLogTimer = undefined;
        }
      };
      request.open('POST', '/api/import');
      request.setRequestHeader('x-file-name', encodeURIComponent(file.name));
      request.upload.onprogress = (event) => {
        if (!event.lengthComputable) {
          setStatus(`正在上传 ${file.name}...`);
          return;
        }
        const percent = Math.round((event.loaded / event.total) * 100);
        setStatus(`正在导入 ${file.name}：${percent}%`);
        if (percent === 1 || percent % 10 === 0 || percent === 100) {
          appendLog(`导入上传 ${percent}% (${formatBytes(event.loaded)} / ${formatBytes(event.total)})`);
        }
      };
      request.upload.onload = () => {
        setStatus('上传完成，正在解析装配层级并生成原模型预览...');
        appendLog('上传完成，开始解析装配层级和原模型预览');
        processingLogTimer = window.setInterval(() => {
          processingSeconds += 20;
          appendLog(`后端处理中：解析装配、三角化、生成原模型预览 (${processingSeconds}s)`);
        }, 20000);
      };
      request.onload = () => {
        clearProcessingLogTimer();
        try {
          const data = JSON.parse(request.responseText || '{}') as ImportResponse;
          if (request.status < 200 || request.status >= 300 || !data.ok) {
            reject(new Error(data.error || `导入失败：HTTP ${request.status}`));
            return;
          }
          resolve(data);
        } catch {
          reject(new Error('导入响应解析失败'));
        }
      };
      request.onerror = () => {
        clearProcessingLogTimer();
        reject(new Error('导入失败：网络或本地服务异常'));
      };
      request.onabort = () => {
        clearProcessingLogTimer();
        reject(new Error('导入已取消'));
      };
      request.send(file);
    });
  }

  async function importModel(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file) return;

    setImporting(true);
    setStatus(`正在导入 ${file.name}...`);
    logIdRef.current = 0;
    setLogs([]);
    appendLog(`开始导入 ${file.name} (${formatBytes(file.size)})`);
    setModelUrl(undefined);
    setExportUrl(undefined);
    setExportName('mesh-simplified.glb');
    setPartExports([]);
    setMetadata(undefined);
    setModelHierarchy(undefined);
    setSelectedNodeIds([]);
    setHiddenNodeIds(new Set());
    setHiddenObjectKeys(new Set());
    setDeletedNodeIds(new Set());
    setDeletedObjectKeys(new Set());
    setDeletedNodeExclusionTokens(new Set());
    try {
      const data = await uploadCadFile(file);
      if (!data.inputPath) throw new Error(data.error || '导入失败');
      setInputPath(data.inputPath);
      setImportedName(data.fileName || file.name);
      setImportedSize(data.size);
      setCacheDir(data.cacheDir || '');
      setMetadata(data.metadata);
      setModelHierarchy(data.metadata?.hierarchy);
      setSelectedNodeIds([]);
      setHiddenNodeIds(new Set());
      setHiddenObjectKeys(new Set());
      setDeletedNodeIds(new Set());
      setDeletedObjectKeys(new Set());
      setDeletedNodeExclusionTokens(new Set());
      setPartExportDialog({
        open: false,
        folderName: defaultPartExportFolderName(data.fileName || file.name, data.inputPath),
        mode: 'batch',
      });
      if (data.modelUrl) {
        setModelUrl(`${data.modelUrl}?t=${Date.now()}`);
        setExportUrl(data.modelUrl);
        setExportName('mesh-original.glb');
      }
      setStatus(`已导入 ${data.fileName || file.name}，已加载原模型`);
      appendLog(`导入完成，后端缓存 ${formatBytes(data.size)}`);
      if (data.modelUrl) appendLog('原模型预览生成完成，已加载到 3D 视图');
      appendLog(`装配层级解析完成：${formatNumber(data.metadata?.partCount)} 部件，${formatNumber(data.metadata?.shapeCount)} 形体`);
    } catch (error) {
      setImportedName('');
      setImportedSize(undefined);
      setCacheDir('');
      setInputPath('');
      setExportUrl(undefined);
      setPartExports([]);
      setModelHierarchy(undefined);
      setSelectedNodeIds([]);
      setHiddenNodeIds(new Set());
      setHiddenObjectKeys(new Set());
      setDeletedNodeIds(new Set());
      setDeletedObjectKeys(new Set());
      setDeletedNodeExclusionTokens(new Set());
      setPartExportDialog({ open: false, folderName: 'model-parts', mode: 'batch' });
      setStatus(error instanceof Error ? error.message : String(error));
      appendLog(`导入失败：${error instanceof Error ? error.message : String(error)}`);
    } finally {
      setImporting(false);
    }
  }

  async function convertModel() {
    setBusy(true);
    setStatus(`正在简化并生成 ${exportFormat.toUpperCase()}...`);
    appendLog('开始解析 CAD、三角化并简化');
    try {
      const response = await fetch('/api/convert', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          inputPath,
          cacheDir,
          ratio,
          linearDeflection,
          error: targetError,
          format: exportFormat,
          excludedNodeIds: [...deletedNodeExclusionTokens],
        }),
      });
      const data = (await response.json()) as ConvertResponse;
      if (!response.ok || !data.ok) throw new Error(data.error || '转换失败');
      const downloadUrl = data.fileUrl || data.modelUrl;
      if (!downloadUrl) throw new Error('转换完成，但后端没有返回下载地址');
      if (data.modelUrl) {
        setModelUrl(`${data.modelUrl}?t=${Date.now()}`);
      }
      setExportUrl(downloadUrl);
      setExportName(`mesh-simplified.${data.format || exportFormat}`);
      setMetadata(data.metadata);
      setModelHierarchy(data.metadata?.hierarchy);
      setSelectedNodeIds([]);
      setHiddenNodeIds(new Set());
      setHiddenObjectKeys(new Set());
      setStatus(data.format === 'stl' ? 'STL 导出完成，3D 视图保留当前预览' : '简化完成，已加载轻量 GLB');
      appendLog(`${(data.format || exportFormat).toUpperCase()} 完成：${formatNumber(data.metadata?.trianglesBefore)} -> ${formatNumber(data.metadata?.trianglesAfter)} 三角面`);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : String(error));
      appendLog(`简化失败：${error instanceof Error ? error.message : String(error)}`);
    } finally {
      setBusy(false);
    }
  }

  function openPartExportDialog() {
    setPartExportDialog((current) => ({
      ...current,
      open: true,
      mode: 'batch',
      sourceNodeId: undefined,
      sourceNodeName: undefined,
      folderName: current.folderName && current.folderName !== 'model-parts' ? current.folderName : defaultExportFolderName,
      directory: canPickExportDirectory ? current.directory : undefined,
      directoryName: canPickExportDirectory ? current.directoryName : '浏览器默认下载目录',
      error: undefined,
    }));
  }

  function openSingleNodeExportDialog(id: string) {
    const node = treeIndex.nodeById.get(id);
    if (!node || node.type === 'root') return;
    const name = node.name || node.id || id;
    setSelectedNodeIds([id]);
    setPartExportDialog((current) => ({
      ...current,
      open: true,
      mode: 'single',
      sourceNodeId: id,
      sourceNodeName: name,
      folderName: `${sanitizeWindowsName(name, 'assembly')}-export`,
      directory: canPickExportDirectory ? current.directory : undefined,
      directoryName: canPickExportDirectory ? current.directoryName : '浏览器默认下载目录',
      error: undefined,
    }));
  }

  async function choosePartExportDirectory() {
    const picker = (window as DirectoryPickerCapableWindow).showDirectoryPicker;
    if (!canPickExportDirectory || !picker) {
      setPartExportDialog((current) => ({
        ...current,
        directory: undefined,
        directoryName: '浏览器默认下载目录',
        error: undefined,
      }));
      return;
    }

    try {
      const directory = await picker({ id: 'mesh-simplifier-part-export', mode: 'readwrite' });
      setPartExportDialog((current) => ({
        ...current,
        directory,
        directoryName: directory.name,
        error: undefined,
      }));
    } catch (error) {
      if (error instanceof DOMException && error.name === 'AbortError') return;
      setPartExportDialog((current) => ({
        ...current,
        error: error instanceof Error ? error.message : String(error),
      }));
    }
  }

  async function savePartExportsToDirectory(files: PartExportFile[], directory: WritableDirectoryHandleLike, folderName: string) {
    const safeFolderName = sanitizeWindowsName(folderName, defaultExportFolderName);
    const targetDirectory = await directory.getDirectoryHandle(safeFolderName, { create: true });
    let savedCount = 0;

    for (const [index, file] of files.entries()) {
      if (!file.url) continue;
      const response = await fetch(file.url);
      if (!response.ok) throw new Error(`下载零件失败：${file.fileName || file.name || response.status}`);
      const blob = await response.blob();
      const fileName = sanitizeWindowsName(file.fileName || file.name || `part-${index + 1}.stl`, `part-${index + 1}.stl`);
      const fileHandle = await targetDirectory.getFileHandle(fileName.toLowerCase().endsWith('.stl') ? fileName : `${fileName}.stl`, { create: true });
      const writable = await fileHandle.createWritable();
      await writable.write(blob);
      await writable.close();
      savedCount += 1;
    }

    return { savedCount, folderName: safeFolderName };
  }

  async function saveSingleExportToDirectory(fileUrl: string, fileName: string, directory: WritableDirectoryHandleLike, folderName: string) {
    const safeFolderName = sanitizeWindowsName(folderName, defaultExportFolderName);
    const targetDirectory = await directory.getDirectoryHandle(safeFolderName, { create: true });
    const response = await fetch(fileUrl);
    if (!response.ok) throw new Error(`下载导出文件失败：HTTP ${response.status}`);
    const blob = await response.blob();
    const safeFileName = sanitizeWindowsName(fileName, 'assembly.stl');
    const fileHandle = await targetDirectory.getFileHandle(safeFileName.toLowerCase().endsWith('.stl') ? safeFileName : `${safeFileName}.stl`, { create: true });
    const writable = await fileHandle.createWritable();
    await writable.write(blob);
    await writable.close();
    return { folderName: safeFolderName, fileName: safeFileName };
  }

  async function exportParts() {
    const folderName = sanitizeWindowsName(partExportDialog.folderName, defaultExportFolderName);
    const directory = partExportDialog.directory;

    setBusy(true);
    const isSingleExport = partExportDialog.mode === 'single' && partExportDialog.sourceNodeId;
    setStatus(isSingleExport ? '正在单独导出 assembly STL...' : '正在批量导出零件 STL...');
    setPartExportDialog((current) => ({ ...current, error: undefined }));
    appendLog(isSingleExport
      ? `开始单独导出 ${partExportDialog.sourceNodeName || partExportDialog.sourceNodeId} STL`
      : `开始批量导出零件 STL，${directory ? `目标文件夹：${folderName}` : '输出为浏览器下载 ZIP'}`);
    try {
      if (isSingleExport) {
        const keepIds = selectedWithDescendants([partExportDialog.sourceNodeId!], treeIndex);
        const includeNodeIds = [...collectNodeIdIncludeTokens(keepIds, modelTreeIndex)];
        if (!includeNodeIds.length) throw new Error('单独导出失败：未找到可导出的真实装配/零件节点');
        const response = await fetch('/api/convert', {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify({
            inputPath,
            cacheDir,
            ratio: 1,
            linearDeflection,
            error: targetError,
            format: 'stl',
            includeNodeIds,
            excludedNodeIds: [...deletedNodeExclusionTokens],
          }),
        });
        const data = (await response.json()) as ConvertResponse;
        if (!response.ok || !data.ok || !data.fileUrl) throw new Error(data.error || '单独导出失败');
        const rawName = partExportDialog.sourceNodeName || partExportDialog.sourceNodeId || 'assembly';
        const fileName = `${sanitizeWindowsName(rawName, 'assembly')}.stl`;
        if (directory) {
          const saved = await saveSingleExportToDirectory(data.fileUrl, fileName, directory, folderName);
          setPartExports([{ name: rawName, fileName: saved.fileName, url: data.fileUrl, triangles: data.metadata?.trianglesAfter }]);
          setPartExportDialog((current) => ({ ...current, open: false, folderName: saved.folderName }));
          setExportUrl(data.fileUrl);
          setExportName(saved.fileName);
          setStatus(`单独导出完成：${saved.fileName}`);
          appendLog(`单独导出完成：${saved.fileName} 已保存到 ${partExportDialog.directoryName || '所选位置'}\\${saved.folderName}`);
        } else {
          setPartExports([{ name: rawName, fileName, url: data.fileUrl, triangles: data.metadata?.trianglesAfter }]);
          setPartExportDialog((current) => ({ ...current, open: false, folderName }));
          setExportUrl(data.fileUrl);
          setExportName(fileName);
          triggerBrowserDownload(data.fileUrl, fileName);
          setStatus(`单独导出完成：${fileName}，已开始下载`);
          appendLog(`单独导出完成：${fileName} 已通过浏览器下载`);
        }
        return;
      }

      const response = await fetch('/api/export-parts', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          inputPath,
          cacheDir,
          ratio: 1,
          linearDeflection,
          error: targetError,
          folderName,
          excludedNodeIds: [...deletedNodeExclusionTokens],
        }),
      });
      const data = (await response.json()) as PartExportResponse;
      if (!response.ok || !data.ok) throw new Error(data.error || '批量导出失败');
      const files = data.files || data.metadata?.files || [];
      if (directory) {
        setStatus(`正在保存 ${files.length} 个 STL 到本地文件夹...`);
        const saved = await savePartExportsToDirectory(files, directory, folderName);
        setPartExports(files);
        setPartExportDialog((current) => ({ ...current, open: false, folderName: saved.folderName }));
        setStatus(`批量导出完成：${saved.savedCount} 个 STL 文件`);
        appendLog(`零件导出完成：${saved.savedCount} 个文件已保存到 ${partExportDialog.directoryName || '所选位置'}\\${saved.folderName}`);
      } else {
        if (!data.zipUrl) throw new Error('批量导出完成，但后端没有返回 ZIP 下载地址');
        const zipFileName = data.zipFileName || `${folderName}.zip`;
        setPartExports(files);
        setPartExportDialog((current) => ({ ...current, open: false, folderName }));
        setExportUrl(data.zipUrl);
        setExportName(zipFileName);
        triggerBrowserDownload(data.zipUrl, zipFileName);
        setStatus(`批量导出完成：${files.length} 个 STL，已开始下载 ZIP`);
        appendLog(`零件导出完成：${files.length} 个文件已打包为 ${zipFileName}`);
      }
    } catch (error) {
      setStatus(error instanceof Error ? error.message : String(error));
      appendLog(`批量导出失败：${error instanceof Error ? error.message : String(error)}`);
      setPartExportDialog((current) => ({
        ...current,
        error: error instanceof Error ? error.message : String(error),
      }));
    } finally {
      setBusy(false);
    }
  }

  const tree = metadata?.hierarchy;
  const treeIndex = useMemo(() => buildTreeIndex(tree), [tree]);
  const modelTreeIndex = useMemo(() => buildTreeIndex(modelHierarchy || tree), [modelHierarchy, tree]);
  const effectiveHiddenNodeIds = useMemo(() => unionSets(hiddenNodeIds, deletedNodeIds), [hiddenNodeIds, deletedNodeIds]);
  const effectiveHiddenObjectKeys = useMemo(() => unionSets(hiddenObjectKeys, deletedObjectKeys), [hiddenObjectKeys, deletedObjectKeys]);
  const selectedIdSet = useMemo(() => selectedWithDescendants(selectedNodeIds, treeIndex), [selectedNodeIds, treeIndex]);
  const visibleSelectedIds = useMemo(
    () => selectedNodeIds.filter((id) => treeIndex.nodeById.has(id) && !effectiveHiddenNodeIds.has(id)),
    [effectiveHiddenNodeIds, selectedNodeIds, treeIndex],
  );
  const canMergeSelection = normalizeMergeIds(visibleSelectedIds, treeIndex).length >= 2;
  const reduction = metadata?.reductionRatio;
  const treeContextNode = treeContextMenu ? treeIndex.nodeById.get(treeContextMenu.id) : undefined;
  const canSplitTreeContextNode = Boolean(treeContextNode?.children?.length && treeContextNode.type !== 'root');

  useEffect(() => {
    setSelectedNodeIds((current) => {
      const next = current.filter((id) => treeIndex.nodeById.has(id) && !effectiveHiddenNodeIds.has(id));
      return next.length === current.length ? current : next;
    });
  }, [effectiveHiddenNodeIds, treeIndex]);

  useEffect(() => {
    const activeId = selectedNodeIds.find((id) => treeIndex.nodeById.has(id));
    if (!activeId) return;
    window.requestAnimationFrame(() => {
      const rows = Array.from(document.querySelectorAll<HTMLElement>('[data-node-id]'));
      const row = rows.find((item) => item.dataset.nodeId === activeId);
      row?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    });
  }, [selectedNodeIds, treeIndex]);

  const selectNode = useCallback((id: string, additive = false) => {
    if (!treeIndex.nodeById.has(id) || effectiveHiddenNodeIds.has(id)) return;
    setSelectedNodeIds((current) => {
      if (!additive) return [id];
      if (current.includes(id)) return current.filter((item) => item !== id);
      return [...current, id];
    });
  }, [effectiveHiddenNodeIds, treeIndex]);

  const selectNodesFromBox = useCallback((ids: string[], additive = false) => {
    const nextIds = [...new Set(ids.filter((id) => treeIndex.nodeById.has(id) && !effectiveHiddenNodeIds.has(id)))];
    setSelectedNodeIds((current) => {
      if (!nextIds.length) return additive ? current : [];
      if (!additive) return nextIds;
      return [...new Set([...current, ...nextIds])];
    });
  }, [effectiveHiddenNodeIds, treeIndex]);

  const clearSelection = useCallback(() => {
    setSelectedNodeIds([]);
  }, []);

  const hideNode = useCallback((id: string) => {
    if (!treeIndex.nodeById.has(id) || deletedNodeIds.has(id)) return;
    const idsToHide = selectedWithDescendants([id], treeIndex);
    if (!idsToHide.size) return;
    const keysToHide = collectNodeKeys(idsToHide, treeIndex);
    setHiddenNodeIds((current) => new Set([...current, ...idsToHide]));
    setHiddenObjectKeys((current) => new Set([...current, ...keysToHide]));
    setSelectedNodeIds((current) => current.filter((selectedId) => !idsToHide.has(selectedId)));
    setStatus(`已隐藏 ${idsToHide.size} 个装配/零件节点`);
    appendLog(`层级显示：隐藏 ${idsToHide.size} 个节点`);
  }, [deletedNodeIds, treeIndex]);

  const showNode = useCallback((id: string) => {
    if (!treeIndex.nodeById.has(id) || deletedNodeIds.has(id)) return;
    const idsToShow = selectedWithDescendants([id], treeIndex);
    const keysToShow = collectNodeKeys(idsToShow, treeIndex);
    setHiddenNodeIds((current) => {
      const next = new Set(current);
      idsToShow.forEach((nodeId) => {
        if (!deletedNodeIds.has(nodeId)) next.delete(nodeId);
      });
      return next;
    });
    setHiddenObjectKeys((current) => {
      const next = new Set(current);
      keysToShow.forEach((key) => {
        if (!deletedObjectKeys.has(key)) next.delete(key);
      });
      return next;
    });
    setStatus(`已显示 ${idsToShow.size} 个装配/零件节点`);
    appendLog(`层级显示：显示 ${idsToShow.size} 个节点`);
  }, [deletedNodeIds, deletedObjectKeys, treeIndex]);

  const isolateNode = useCallback((id: string) => {
    if (!treeIndex.nodeById.has(id) || deletedNodeIds.has(id)) return;
    const keepIds = selectedWithDescendants([id], treeIndex);
    const idsToHide = new Set<string>();
    treeIndex.nodeById.forEach((_node, id) => {
      if (!keepIds.has(id) && !deletedNodeIds.has(id)) idsToHide.add(id);
    });
    const keysToHide = collectNodeKeys(idsToHide, treeIndex);
    setHiddenNodeIds(idsToHide);
    setHiddenObjectKeys(keysToHide);
    setSelectedNodeIds([id]);
    setStatus(`仅显示 ${keepIds.size} 个装配/零件节点`);
    appendLog(`层级显示：仅显示 ${id}，隐藏 ${idsToHide.size} 个节点`);
  }, [deletedNodeIds, treeIndex]);

  const deleteSelectedNodes = useCallback(() => {
    if (!visibleSelectedIds.length) return;
    const idsToRemove = selectedWithDescendants(visibleSelectedIds, treeIndex);
    if (!idsToRemove.size) return;
    const keysToHide = collectNodeKeys(idsToRemove, treeIndex);
    const tokensToExclude = collectNodeExclusionTokens(idsToRemove, treeIndex);
    setMetadata((current) => current ? { ...current, hierarchy: removeNodesFromTree(current.hierarchy, idsToRemove) } : current);
    setHiddenNodeIds((current) => new Set([...current, ...idsToRemove]));
    setHiddenObjectKeys((current) => new Set([...current, ...keysToHide]));
    setDeletedNodeIds((current) => new Set([...current, ...idsToRemove]));
    setDeletedObjectKeys((current) => new Set([...current, ...keysToHide]));
    setDeletedNodeExclusionTokens((current) => new Set([...current, ...tokensToExclude]));
    setSelectedNodeIds([]);
    setStatus(`已删除 ${idsToRemove.size} 个装配/零件节点`);
    appendLog(`层级编辑：删除/隐藏 ${idsToRemove.size} 个节点`);
  }, [treeIndex, visibleSelectedIds]);

  const deleteNode = useCallback((id: string) => {
    if (!treeIndex.nodeById.has(id) || deletedNodeIds.has(id)) return;
    const idsToRemove = selectedWithDescendants([id], treeIndex);
    if (!idsToRemove.size) return;
    const keysToHide = collectNodeKeys(idsToRemove, treeIndex);
    const tokensToExclude = collectNodeExclusionTokens(idsToRemove, treeIndex);
    setMetadata((current) => current ? { ...current, hierarchy: removeNodesFromTree(current.hierarchy, idsToRemove) } : current);
    setHiddenNodeIds((current) => new Set([...current, ...idsToRemove]));
    setHiddenObjectKeys((current) => new Set([...current, ...keysToHide]));
    setDeletedNodeIds((current) => new Set([...current, ...idsToRemove]));
    setDeletedObjectKeys((current) => new Set([...current, ...keysToHide]));
    setDeletedNodeExclusionTokens((current) => new Set([...current, ...tokensToExclude]));
    setSelectedNodeIds([]);
    setStatus(`已删除 ${idsToRemove.size} 个装配/零件节点`);
    appendLog(`层级编辑：右键删除 ${idsToRemove.size} 个节点`);
  }, [deletedNodeIds, treeIndex]);

  const mergeSelectedNodes = useCallback(() => {
    const mergeIds = normalizeMergeIds(visibleSelectedIds, treeIndex);
    if (mergeIds.length < 2) return;
    const nextCounter = mergeCounter + 1;
    const groupId = `merged-${Date.now()}-${nextCounter}`;
    setMergeCounter(nextCounter);
    setMetadata((current) => current ? { ...current, hierarchy: mergeNodesInTree(current.hierarchy, mergeIds, treeIndex, groupId) } : current);
    setSelectedNodeIds([groupId]);
    setStatus(`已合并 ${mergeIds.length} 个装配/零件节点`);
    appendLog(`层级编辑：合并 ${mergeIds.length} 项为新 assembly`);
  }, [mergeCounter, treeIndex, visibleSelectedIds]);

  const splitNode = useCallback((id: string) => {
    const node = treeIndex.nodeById.get(id);
    if (!node?.children?.length || node.type === 'root') return;
    setMetadata((current) => current ? { ...current, hierarchy: splitNodeInTree(current.hierarchy, id) } : current);
    setSelectedNodeIds(node.children.map((child) => nodeId(child)));
    setStatus(`已拆分 ${node.name || node.id || id}`);
    appendLog(`层级编辑：拆分 ${node.name || node.id || id}`);
  }, [treeIndex]);

  const renameSelectedNode = useCallback((id: string, name: string) => {
    if (!treeIndex.nodeById.has(id)) return;
    setMetadata((current) => current ? { ...current, hierarchy: renameNodeInTree(current.hierarchy, id, name) } : current);
    setSelectedNodeIds([id]);
    setStatus(`已重命名为 ${name}`);
    appendLog(`层级编辑：重命名节点为 ${name}`);
  }, [treeIndex]);

  function openTreeContextMenu(id: string, event: React.MouseEvent<HTMLElement>) {
    event.preventDefault();
    event.stopPropagation();
    if (!treeIndex.nodeById.has(id) || deletedNodeIds.has(id)) return;
    setSelectedNodeIds((current) => current.includes(id) ? current : [id]);
    setTreeContextMenu({
      id,
      x: Math.min(event.clientX, window.innerWidth - 178),
      y: Math.min(event.clientY, window.innerHeight - 230),
    });
  }

  function renameFromTreeMenu() {
    if (!treeContextMenu) return;
    const node = treeIndex.nodeById.get(treeContextMenu.id);
    const currentName = node?.name || node?.id || treeContextMenu.id;
    const nextName = window.prompt('重命名装配/零件', currentName);
    if (nextName?.trim()) renameSelectedNode(treeContextMenu.id, nextName.trim());
    setTreeContextMenu(undefined);
  }

  return (
    <main className="app-shell">
      <aside className="left-panel glass-panel">
        <div className="brand-row">
          <div className="brand-mark"><Layers3 size={18} /></div>
          <div>
            <strong>Mesh Simplifier</strong>
            <span>STEP / IGES to GLB / STL</span>
          </div>
        </div>

        <section className={collapsedSections.input ? 'panel-section collapsed' : 'panel-section'}>
          <button
            type="button"
            className={collapsedSections.input ? 'section-title collapsible-title collapsed' : 'section-title collapsible-title'}
            aria-expanded={!collapsedSections.input}
            onClick={() => toggleSection('input')}
          >
            <span className="title-left">
              <FolderOpen size={15} />
              <span>模型输入</span>
            </span>
            <ChevronRight className={collapsedSections.input ? '' : 'expanded'} size={15} />
          </button>
          {!collapsedSections.input && (
            <div className="section-content">
              <label className="path-input">
                <span>已导入模型</span>
                <input
                  value={inputPath}
                  readOnly
                  placeholder="先点击导入模型"
                />
              </label>
              {importedName && <p className="import-note">当前文件：{importedName} · {formatBytes(importedSize)}</p>}
              <input
                ref={fileInputRef}
                className="hidden-file-input"
                type="file"
                accept=".step,.stp,.iges,.igs"
                onChange={importModel}
              />
              <button className="secondary-button" disabled={busy || importing} onClick={() => fileInputRef.current?.click()}>
                {importing ? <Loader2 className="spin" size={16} /> : <FolderOpen size={16} />}
                <span>{importing ? '导入中' : '导入模型'}</span>
              </button>
              <button className="primary-button" disabled={busy || importing || !inputPath.trim()} onClick={convertModel}>
                {busy ? <Loader2 className="spin" size={16} /> : <Play size={16} />}
                <span>{busy ? '简化中' : '开始简化'}</span>
              </button>
              <button className="secondary-button" disabled={busy || importing || !inputPath.trim()} onClick={openPartExportDialog}>
                {busy ? <Loader2 className="spin" size={16} /> : <FileDown size={16} />}
                <span>批量导出零件 STL</span>
              </button>
            </div>
          )}
        </section>

        <section className={collapsedSections.parameters ? 'panel-section collapsed' : 'panel-section'}>
          <button
            type="button"
            className={collapsedSections.parameters ? 'section-title collapsible-title collapsed' : 'section-title collapsible-title'}
            aria-expanded={!collapsedSections.parameters}
            onClick={() => toggleSection('parameters')}
          >
            <span className="title-left">
              <SlidersHorizontal size={15} />
              <span>简化参数</span>
            </span>
            <ChevronRight className={collapsedSections.parameters ? '' : 'expanded'} size={15} />
          </button>
          {!collapsedSections.parameters && (
            <div className="section-content">
              <label className="range-row">
                <span>目标比例</span>
                <input type="range" min="0.05" max="1" step="0.05" value={ratio} onChange={(e) => setRatio(Number(e.target.value))} />
                <b>{Math.round(ratio * 100)}%</b>
              </label>
              <label className="range-row">
                <span>三角化精度</span>
                <input type="range" min="0.05" max="2" step="0.05" value={linearDeflection} onChange={(e) => setLinearDeflection(Number(e.target.value))} />
                <b>{linearDeflection.toFixed(2)}</b>
              </label>
              <label className="range-row">
                <span>误差阈值</span>
                <input type="range" min="0.005" max="0.2" step="0.005" value={targetError} onChange={(e) => setTargetError(Number(e.target.value))} />
                <b>{targetError.toFixed(3)}</b>
              </label>
              <label className="format-row">
                <span>导出格式</span>
                <select value={exportFormat} onChange={(event) => setExportFormat(event.target.value as 'glb' | 'stl')}>
                  <option value="glb">GLB</option>
                  <option value="stl">STL</option>
                </select>
              </label>
            </div>
          )}
        </section>

        <section className={collapsedSections.tree ? 'panel-section tree-section collapsed' : 'panel-section tree-section'}>
          <button
            type="button"
            className={collapsedSections.tree ? 'section-title tree-title collapsible-title collapsed' : 'section-title tree-title collapsible-title'}
            aria-expanded={!collapsedSections.tree}
            onClick={() => toggleSection('tree')}
          >
            <span className="title-left">
              <Layers3 size={15} />
              <span>装配层级</span>
            </span>
            <span className="section-title-actions">
              <span className="selection-pill">
                <MousePointer2 size={12} />
                {selectedIdSet.size ? `已选 ${selectedIdSet.size}` : '左键点选 / 拖框'}
              </span>
              <ChevronRight className={collapsedSections.tree ? '' : 'expanded'} size={15} />
            </span>
          </button>
          {!collapsedSections.tree && (
            <>
              <div className="tree-actions">
                <button className="mini-action danger" disabled={busy || importing || !visibleSelectedIds.length} onClick={deleteSelectedNodes}>
                  <Trash2 size={13} />
                  删除
                </button>
                <button className="mini-action" disabled={busy || importing || !canMergeSelection} onClick={mergeSelectedNodes}>
                  <GitMerge size={13} />
                  合并
                </button>
                <button className="mini-action ghost" disabled={!visibleSelectedIds.length} onClick={clearSelection}>
                  清空
                </button>
              </div>
              <div className="tree-scroll">
                {hasTreeContent(tree) ? (
                  <TreeView
                    node={tree}
                    selectedIds={selectedIdSet}
                    hiddenIds={hiddenNodeIds}
                    onSelect={selectNode}
                    onContextMenu={openTreeContextMenu}
                    onHide={hideNode}
                    onShow={showNode}
                    onIsolate={isolateNode}
                  />
                ) : (
                  <p className="empty-note">导入模型后显示从 STEP / IGES 解析出的装配层级。</p>
                )}
              </div>
            </>
          )}
        </section>
      </aside>

      {treeContextMenu && (
        <div
          className="viewport-context-menu tree-context-menu"
          style={{ position: 'fixed', left: treeContextMenu.x, top: treeContextMenu.y }}
          onPointerDown={(event) => event.stopPropagation()}
          onContextMenu={(event) => event.preventDefault()}
        >
          <button onClick={renameFromTreeMenu}>
            <Pencil size={13} />
            重命名
          </button>
          <button
            disabled={!treeContextNode || treeContextNode.type === 'root'}
            onClick={() => {
              openSingleNodeExportDialog(treeContextMenu.id);
              setTreeContextMenu(undefined);
            }}
          >
            <FileDown size={13} />
            单独导出
          </button>
          <button
            onClick={() => {
              if (hiddenNodeIds.has(treeContextMenu.id)) showNode(treeContextMenu.id);
              else hideNode(treeContextMenu.id);
              setTreeContextMenu(undefined);
            }}
          >
            {hiddenNodeIds.has(treeContextMenu.id) ? <Eye size={13} /> : <EyeOff size={13} />}
            {hiddenNodeIds.has(treeContextMenu.id) ? '显示' : '隐藏'}
          </button>
          <button
            onClick={() => {
              isolateNode(treeContextMenu.id);
              setTreeContextMenu(undefined);
            }}
          >
            <Eye size={13} />
            仅显
          </button>
          <button
            onClick={() => {
              deleteNode(treeContextMenu.id);
              setTreeContextMenu(undefined);
            }}
          >
            <Trash2 size={13} />
            删除
          </button>
          <button
            disabled={!canMergeSelection}
            onClick={() => {
              mergeSelectedNodes();
              setTreeContextMenu(undefined);
            }}
          >
            <GitMerge size={13} />
            合并
          </button>
          <button
            disabled={!canSplitTreeContextNode}
            onClick={() => {
              splitNode(treeContextMenu.id);
              setTreeContextMenu(undefined);
            }}
          >
            <Split size={13} />
            拆分
          </button>
        </div>
      )}

      <section className="main-stage">
        <header className="top-bar glass-panel">
          <div>
            <h1>工作台</h1>
            <p>{status}</p>
          </div>
          <div className="toolbar">
            <button onClick={() => setWireframe((value) => !value)} className={wireframe ? 'tool active' : 'tool'} title="线框显示">
              <ScanLine size={16} />
            </button>
            <button
              className="tool"
              title="适配视图"
              disabled={!modelUrl}
              onClick={() => setViewCommand({ token: Date.now() })}
            >
              <Maximize2 size={16} />
            </button>
            <a className={exportUrl ? 'export-link' : 'export-link disabled'} href={exportUrl} download={exportName}>
              <FileDown size={16} />
              <span>下载 {exportName.endsWith('.stl') ? 'STL' : 'GLB'}</span>
            </a>
          </div>
        </header>

        <div className="viewport-card">
          <div className="viewport-badges">
            <span><Eye size={13} /> Three.js Preview</span>
            <span><Sparkles size={13} /> WebGL</span>
            <span><MousePointer2 size={13} /> 左键选取 · 右键旋转/菜单</span>
          </div>
          <SceneViewport
            modelUrl={modelUrl}
            wireframe={wireframe}
            viewCommand={viewCommand}
            treeIndex={treeIndex}
            modelTreeIndex={modelTreeIndex}
            selectedNodeIds={selectedNodeIds}
            hiddenNodeIds={effectiveHiddenNodeIds}
            hiddenObjectKeys={effectiveHiddenObjectKeys}
            onSelectNode={selectNode}
            onBoxSelect={selectNodesFromBox}
            onRenameNode={renameSelectedNode}
            onDeleteSelected={deleteSelectedNodes}
            onMergeSelected={mergeSelectedNodes}
            onSplitNode={splitNode}
            onExportNode={openSingleNodeExportDialog}
            onHideNode={hideNode}
            onShowNode={showNode}
            onIsolateNode={isolateNode}
            canMergeSelection={canMergeSelection}
            onClearSelection={clearSelection}
          />
        </div>
      </section>

      <aside className="right-panel glass-panel">
        <section className="metric-hero">
          <div>
            <span>压缩率</span>
            <strong>{formatPercent(reduction)}</strong>
          </div>
          <Gauge size={38} />
        </section>
        <div className="metrics-grid">
          <div><span>部件</span><strong>{formatNumber(metadata?.partCount)}</strong></div>
          <div><span>形体</span><strong>{formatNumber(metadata?.shapeCount)}</strong></div>
          <div><span>原三角面</span><strong>{formatNumber(metadata?.trianglesBefore)}</strong></div>
          <div><span>新三角面</span><strong>{formatNumber(metadata?.trianglesAfter)}</strong></div>
        </div>

        <section className="panel-section">
          <div className="section-title">
            <CircleCheck size={15} />
            <span>过程日志</span>
          </div>
          <div className="process-log">
            {logs.map((item) => (
              <span key={item.id}>{item.text}</span>
            ))}
          </div>
        </section>

        <section className="panel-section shape-list">
          <div className="section-title">
            <Layers3 size={15} />
            <span>形体统计</span>
          </div>
          {(metadata?.shapes ?? []).slice(0, 12).map((shape) => (
            <div className="shape-row" key={shape.key}>
              <span>{shape.key}</span>
              <b>{formatNumber(shape.trianglesAfter)}</b>
            </div>
          ))}
          {!metadata?.shapes?.length && (
            <p className="empty-note">转换完成后显示每个形体的简化结果。</p>
          )}
        </section>

        <section className="panel-section shape-list">
          <div className="section-title">
            <FileDown size={15} />
            <span>零件导出</span>
          </div>
          {partExports.slice(0, 12).map((file, index) => (
            <a className="shape-row export-row" href={file.url} download={file.fileName || `part-${index + 1}.stl`} key={`${file.fileName || file.name}-${index}`}>
              <span>{file.fileName || file.name || `part-${index + 1}.stl`}</span>
              <b>{formatNumber(file.triangles)}</b>
            </a>
          ))}
          {!partExports.length && (
            <p className="empty-note">批量导出后，这里显示新建文件夹里的零件 STL。</p>
          )}
        </section>
      </aside>

      {partExportDialog.open && (
        <div className="modal-backdrop" role="presentation" onMouseDown={(event) => {
          if (event.target === event.currentTarget && !busy) {
            setPartExportDialog((current) => ({ ...current, open: false, error: undefined }));
          }
        }}>
          <section className="export-dialog" role="dialog" aria-modal="true" aria-labelledby="part-export-title">
            <div className="dialog-header">
              <div>
                <h2 id="part-export-title">{partExportDialog.mode === 'single' ? '单独导出 STL' : '批量导出零件 STL'}</h2>
                <p>
                  {partExportDialog.mode === 'single'
                    ? `将 ${partExportDialog.sourceNodeName || '当前 assembly/part'} 作为整体 STL 导出。`
                    : '批量导出会生成单独零件；远程访问时会自动打包为 ZIP 下载。'}
                </p>
              </div>
              <FileDown size={22} />
            </div>

            <label className="dialog-field">
              <span>保存位置</span>
              <div className="directory-row">
                <input value={partExportDialog.directoryName || (canPickExportDirectory ? '未选择文件夹，默认浏览器下载' : '浏览器默认下载目录')} readOnly />
                <button
                  type="button"
                  disabled={busy || !canPickExportDirectory}
                  title={canPickExportDirectory ? '选择本地保存文件夹' : '当前 HTTP 局域网访问不支持选择本地文件夹'}
                  onClick={choosePartExportDirectory}
                >
                  <FolderOpen size={14} />
                  {canPickExportDirectory ? '浏览' : '不可用'}
                </button>
              </div>
            </label>
            {!canPickExportDirectory && (
              <p className="dialog-hint">当前通过局域网 HTTP 访问，浏览器不允许网页选择本地文件夹；导出结果会通过普通下载保存。</p>
            )}

            <label className="dialog-field">
              <span>新建文件夹名称</span>
              <input
                value={partExportDialog.folderName}
                disabled={busy}
                onChange={(event) => setPartExportDialog((current) => ({ ...current, folderName: event.target.value, error: undefined }))}
              />
            </label>

            {partExportDialog.error && <p className="dialog-error">{partExportDialog.error}</p>}

            <div className="dialog-actions">
              <button
                type="button"
                className="dialog-cancel"
                disabled={busy}
                onClick={() => setPartExportDialog((current) => ({ ...current, open: false, error: undefined }))}
              >
                取消
              </button>
              <button
                type="button"
                className="dialog-confirm"
                disabled={busy || importing || !partExportDialog.folderName.trim()}
                onClick={exportParts}
              >
                {busy ? <Loader2 className="spin" size={15} /> : <FileDown size={15} />}
                {partExportDialog.directory
                  ? (partExportDialog.mode === 'single' ? '导出当前项' : '开始导出')
                  : (partExportDialog.mode === 'single' ? '下载当前项' : '下载 ZIP')}
              </button>
            </div>
          </section>
        </div>
      )}
    </main>
  );
}
