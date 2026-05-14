# Task Authoring Frontend Contract

Phase 68 exposes plugin-neutral authoring documents to frontend clients. A frontend renders `AuthoringDocument` JSON, sends `AuthoringChange` JSON, and treats returned accepted settings plus authoring metadata as the persisted business truth.

## AuthoringDocument

Required fields are lower camelCase:

- `version`: contract version.
- `kind`: `formSequence` or `graph`; `customWeb` is deferred.
- `revision`: optimistic edit revision.
- `values`: accepted task settings and authoring metadata projection.
- `view`: renderer layout hints.
- `schema`: field, sequence, node, port, and connection schema data.
- `catalog`: available actions and component definitions.
- `state`: transient server-provided render state.
- `diagnostics`: validation and migration diagnostics.
- `migration`: stable IDs, orphan handling, and diff or upgrade notes.

`formSequence` is the generic projection used by Maa PI for project folder, controller/resource selection, preset choice, task catalog, task sequence, and nested options. The frontend must not consume plugin-private Maa PI protocols.

`graph` uses task component definitions and ActionCatalog entries for nodes, ports, connections, flow-control actions, diagnostics, and migration state.

## AuthoringChange

Every change request contains:

- `baseRevision`: the document revision observed by the client.
- `kind`: one of `setValue`, `applyPreset`, `addSequenceItem`, `moveSequenceItem`, `addNode`, `connectPorts`, or `updateNodeConfig`.
- `payload`: operation-specific JSON.

The server validates the envelope before passing it to plugin authoring code. Successful apply responses include accepted settings and a refreshed `AuthoringDocument`.

## HTTP Shapes

Instance-level authoring endpoints use DAS envelopes:

- `POST /scheduler/task/{taskId}/authoring/get`
- `POST /scheduler/task/{taskId}/authoring/apply`
- `POST /scheduler/task/{taskId}/authoring/compile`

Responses use `errorKind` for deterministic frontend handling. Planned error kinds include `missingField`, `unsupportedKind`, `unsupportedChange`, `invalidRevision`, `compileFailed`, and `pluginUnavailable`.
