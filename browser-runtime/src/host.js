import {
  buildComponentRpcUrl,
  buildDataRpcPayload,
  buildDataRpcUrl,
  normalizeComponentRef,
} from './runtime-contract.js';

function normalizeJsonData(req, data) {
  if (req?.isSingle && Array.isArray(data)) {
    return data[0] || null;
  }
  return data;
}

export function createBrowserHost(options = {}) {
  const {
    originUrl,
    pageName,
    fetchImpl = globalThis.fetch,
  } = options;

  if (!originUrl) {
    throw new Error('createBrowserHost requires originUrl');
  }
  if (!pageName) {
    throw new Error('createBrowserHost requires pageName');
  }
  if (typeof fetchImpl !== 'function') {
    throw new Error('createBrowserHost requires a fetch implementation');
  }

  return {
    async fetchData(req, params = {}) {
      const response = await fetchImpl(buildDataRpcUrl(originUrl, pageName), {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(buildDataRpcPayload(req, params)),
      });

      if (!response.ok) {
        const detail = await response.text().catch(() => '');
        throw new Error(
          `Browser data RPC failed: status=${response.status} body=${detail}`,
        );
      }

      const json = await response.json();
      return normalizeJsonData(req, json);
    },

    async loadComponentBytecode(ref) {
      const normalizedRef = normalizeComponentRef(ref);
      const response = await fetchImpl(buildComponentRpcUrl(originUrl, normalizedRef.path));
      if (!response.ok) {
        const detail = await response.text().catch(() => '');
        throw new Error(
          `Browser component RPC failed: path=${normalizedRef.path} status=${response.status} body=${detail}`,
        );
      }
      return new Uint8Array(await response.arrayBuffer());
    },
  };
}
