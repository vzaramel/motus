/**
 * Browser runtime contract helpers.
 *
 * Must remain parity-compatible with edge/runtime-wasm helpers.
 */

export function normalizeU32(value) {
  return Number(value) >>> 0;
}

function normalizePathSeparators(path) {
  return String(path ?? '').replace(/\\/g, '/');
}

function trimSlashes(path) {
  return path.replace(/^\/+|\/+$/g, '');
}

function assertSafeComponentSubpath(path) {
  if (!path) {
    throw new Error('Component path is required');
  }
  if (path.includes('\0')) {
    throw new Error('Component path contains NUL byte');
  }
  if (/^([a-zA-Z]:)?\//.test(path)) {
    throw new Error('Component path must be relative');
  }
  if (/(\.\.?)(\/|$)/.test(path) && /(^|\/)\.\.?(\/|$)/.test(path)) {
    throw new Error('Component path cannot contain dot segments');
  }
  if (path.includes('//')) {
    throw new Error('Component path cannot contain empty segments');
  }
}

export function normalizeDataRequirement(req) {
  return {
    queryRef: normalizeU32(req?.queryRef ?? 0),
    signature: normalizeU32(req?.signature ?? 0),
    isSingle: !!req?.isSingle,
    name: req?.name ? String(req.name) : '',
  };
}

export function buildDataRpcUrl(originUrl, pageName) {
  return `${originUrl}/data/${encodeURIComponent(pageName)}`;
}

export function buildDataRpcPayload(req, params) {
  const normalized = normalizeDataRequirement(req);
  const payloadParams =
    params && typeof params === 'object' && !Array.isArray(params) ? params : {};

  return {
    queryRef: normalized.queryRef,
    signature: normalized.signature,
    params: payloadParams,
    single: normalized.isSingle,
  };
}

export function normalizeComponentPath(componentPath, options = {}) {
  const namespace = String(options.defaultNamespace || 'components').replace(/\/+$/g, '');
  const raw = trimSlashes(normalizePathSeparators(componentPath));
  assertSafeComponentSubpath(raw);

  if (raw === namespace || raw.startsWith(`${namespace}/`)) {
    return raw;
  }
  return `${namespace}/${raw}`;
}

export function normalizeComponentRef(ref, options = {}) {
  const path = normalizeComponentPath(ref?.path || ref?.name || '', options);
  const fallbackName = path.split('/').pop() || '';
  return {
    name: ref?.name ? String(ref.name) : fallbackName,
    path,
  };
}

export function componentRouteName(componentPath, options = {}) {
  const namespace = String(options.namespace || options.defaultNamespace || 'components').replace(/\/+$/g, '');
  const normalized = normalizeComponentPath(componentPath, { defaultNamespace: namespace });
  if (normalized === namespace) return '';
  if (normalized.startsWith(`${namespace}/`)) {
    return normalized.slice(namespace.length + 1);
  }
  return normalized;
}

export function buildComponentRpcUrl(originUrl, componentPath, options = {}) {
  const route = componentRouteName(componentPath, options);
  return `${originUrl}/component/${encodeURIComponent(route)}`;
}
