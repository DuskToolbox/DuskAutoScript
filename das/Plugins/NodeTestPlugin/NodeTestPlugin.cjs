"use strict";

const das = require("das-core-node");

const {
  DasPluginFeature,
  DasResult,
  DasVariantType,
} = das;

const COMPONENT_IID = das.guid("15FF0855-E031-4602-829D-040230515C55");
const COMPONENT_FACTORY_IID = das.guid("104C288C-5970-40B9-8E3F-B0B7E4ED509A");

const OK = DasResult.DAS_S_OK;
const OUT_OF_RANGE = DasResult.DAS_E_OUT_OF_RANGE;
const FEATURE_COMPONENT_FACTORY =
  DasPluginFeature.DAS_PLUGIN_FEATURE_COMPONENT_FACTORY;

function toIndex(index) {
  return Number(index);
}

async function wrapComponentAsync(nativeComponent) {
  if (!nativeComponent || typeof nativeComponent !== "object") {
    return nativeComponent;
  }
  if (typeof nativeComponent.dispatchAsync === "function") {
    return nativeComponent;
  }
  return new das.IDasComponent(nativeComponent);
}

async function wrapVariantVectorAsync(args) {
  if (!args || typeof args !== "object") {
    return args;
  }
  if (typeof args.getStringAsync === "function") {
    return args;
  }
  return new das.IDasVariantVector(args);
}

async function readStringAsync(args, index) {
  const vector = await wrapVariantVectorAsync(args);
  return vector.getStringAsync(BigInt(index));
}

async function readIntAsync(args, index) {
  const vector = await wrapVariantVectorAsync(args);
  return Number(await vector.getIntAsync(BigInt(index)));
}

function makeVariantVector(items = []) {
  const values = items.slice();

  function at(index) {
    const value = values[toIndex(index)];
    if (!value) {
      throw new Error("NodeTestPlugin variant index out of range");
    }
    return value;
  }

  function set(index, type, value) {
    values[toIndex(index)] = { type, value };
    return OK;
  }

  function push(type, value) {
    values.push({ type, value });
    return OK;
  }

  return new das.INapiDasVariantVector({
    getInt(index) {
      const item = at(index);
      if (item.type !== DasVariantType.DAS_VARIANT_TYPE_INT) {
        throw new Error("NodeTestPlugin variant is not int");
      }
      return BigInt(item.value);
    },
    getFloat(index) {
      const item = at(index);
      if (item.type !== DasVariantType.DAS_VARIANT_TYPE_FLOAT) {
        throw new Error("NodeTestPlugin variant is not float");
      }
      return Number(item.value);
    },
    getString(index) {
      const item = at(index);
      if (item.type !== DasVariantType.DAS_VARIANT_TYPE_STRING) {
        throw new Error("NodeTestPlugin variant is not string");
      }
      return String(item.value);
    },
    getBool(index) {
      const item = at(index);
      if (item.type !== DasVariantType.DAS_VARIANT_TYPE_BOOL) {
        throw new Error("NodeTestPlugin variant is not bool");
      }
      return Boolean(item.value);
    },
    getComponent(index) {
      const item = at(index);
      if (item.type !== DasVariantType.DAS_VARIANT_TYPE_COMPONENT) {
        throw new Error("NodeTestPlugin variant is not component");
      }
      return item.value;
    },
    getBase(index) {
      const item = at(index);
      if (item.type !== DasVariantType.DAS_VARIANT_TYPE_BASE) {
        throw new Error("NodeTestPlugin variant is not base");
      }
      return item.value;
    },
    setInt(index, value) {
      return set(index, DasVariantType.DAS_VARIANT_TYPE_INT, BigInt(value));
    },
    setFloat(index, value) {
      return set(index, DasVariantType.DAS_VARIANT_TYPE_FLOAT, Number(value));
    },
    setString(index, value) {
      return set(index, DasVariantType.DAS_VARIANT_TYPE_STRING, String(value));
    },
    setBool(index, value) {
      return set(index, DasVariantType.DAS_VARIANT_TYPE_BOOL, Boolean(value));
    },
    setComponent(index, value) {
      return set(index, DasVariantType.DAS_VARIANT_TYPE_COMPONENT, value);
    },
    setBase(index, value) {
      return set(index, DasVariantType.DAS_VARIANT_TYPE_BASE, value);
    },
    pushBackInt(value) {
      return push(DasVariantType.DAS_VARIANT_TYPE_INT, BigInt(value));
    },
    pushBackFloat(value) {
      return push(DasVariantType.DAS_VARIANT_TYPE_FLOAT, Number(value));
    },
    pushBackString(value) {
      return push(DasVariantType.DAS_VARIANT_TYPE_STRING, String(value));
    },
    pushBackBool(value) {
      return push(DasVariantType.DAS_VARIANT_TYPE_BOOL, Boolean(value));
    },
    pushBackComponent(value) {
      return push(DasVariantType.DAS_VARIANT_TYPE_COMPONENT, value);
    },
    pushBackBase(value) {
      return push(DasVariantType.DAS_VARIANT_TYPE_BASE, value);
    },
    getType(index) {
      return at(index).type;
    },
    removeAt(index) {
      const itemIndex = toIndex(index);
      if (itemIndex < 0 || itemIndex >= values.length) {
        return OUT_OF_RANGE;
      }
      values.splice(itemIndex, 1);
      return OK;
    },
    getSize() {
      return values.length;
    },
  });
}

function stringVector(...values) {
  return makeVariantVector(
    values.map((value) => ({
      type: DasVariantType.DAS_VARIANT_TYPE_STRING,
      value,
    })),
  );
}

function intVector(value) {
  return makeVariantVector([
    {
      type: DasVariantType.DAS_VARIANT_TYPE_INT,
      value: BigInt(value),
    },
  ]);
}

function makeLifecycleDirector() {
  return new das.INapiDasComponent({
    getGuid() {
      return COMPONENT_IID;
    },
    getRuntimeClassName() {
      return "Das.NodeBridgeLifecycleDirector";
    },
    dispatch(functionName) {
      if (functionName === "getSessionInfo") {
        return stringVector("Node", "NodeTestPlugin", "Director");
      }
      if (functionName === "getSessionInfoPromise") {
        return Promise.resolve(
          stringVector("Node", "NodeTestPlugin", "DirectorPromise"),
        );
      }
      throw new Error(`Node lifecycle director does not implement ${functionName}`);
    },
  });
}

function createComponent(sessionId) {
  return new das.INapiDasComponent({
    getGuid() {
      return COMPONENT_IID;
    },
    getRuntimeClassName() {
      return "Das.NodeTestPlugin.Component";
    },
    async dispatch(functionName, args) {
      switch (functionName) {
        case "getSessionInfo":
          return makeVariantVector([
            {
              type: DasVariantType.DAS_VARIANT_TYPE_INT,
              value: BigInt(sessionId),
            },
            {
              type: DasVariantType.DAS_VARIANT_TYPE_STRING,
              value: "Node",
            },
            {
              type: DasVariantType.DAS_VARIANT_TYPE_STRING,
              value: "NodeTestPlugin",
            },
          ]);
        case "echo": {
          const input = await readStringAsync(args, 0);
          return stringVector(`[Node] echo: ${input}`);
        }
        case "compute": {
          const operation = await readStringAsync(args, 0);
          const left = await readIntAsync(args, 1);
          const right = await readIntAsync(args, 2);
          if (operation === "add") {
            return intVector(left + right);
          }
          if (operation === "sub") {
            return intVector(left - right);
          }
          if (operation === "mul") {
            return intVector(left * right);
          }
          if (operation === "div" && right !== 0) {
            return intVector(Math.trunc(left / right));
          }
          throw new Error(`NodeTestPlugin compute invalid operation: ${operation}`);
        }
        case "bridgeLifecycleTest": {
          const normalizedArgs = await wrapVariantVectorAsync(args);
          const callback = await wrapComponentAsync(
            await normalizedArgs.getComponentAsync(0n),
          );
          const marker = await readStringAsync(args, 1);
          const callbackArgs = makeVariantVector();
          await callbackArgs.pushBackStringAsync(`bridge_released:Node:${marker}`);
          setTimeout(async () => {
            try {
              await callback.dispatchAsync("lifecycle_callback", callbackArgs);
            } catch (error) {
              process.stderr.write(
                `Node lifecycle callback dispatch failed: ${error.message}\n`,
              );
            }
          }, 100);

          const director = makeLifecycleDirector();
          return makeVariantVector([
            {
              type: DasVariantType.DAS_VARIANT_TYPE_STRING,
              value: `director_created:${marker}`,
            },
            {
              type: DasVariantType.DAS_VARIANT_TYPE_COMPONENT,
              value: director,
            },
          ]);
        }
        default:
          throw new Error(`NodeTestPlugin unknown dispatch method: ${functionName}`);
      }
    },
  });
}

function createFactory(sessionId) {
  return new das.INapiDasComponentFactory({
    getGuid() {
      return COMPONENT_FACTORY_IID;
    },
    getRuntimeClassName() {
      return "Das.NodeTestPlugin.ComponentFactory";
    },
    isSupported() {
      return OK;
    },
    createInstance() {
      return createComponent(sessionId);
    },
  });
}

function createPlugin() {
  let sessionId = 0;

  const plugin = new das.INapiDasPluginPackage({
    setSessionId(value) {
      sessionId = Number(value);
      return OK;
    },
    enumFeature(index) {
      if (index === 0n) {
        return FEATURE_COMPONENT_FACTORY;
      }
      throw new Error(`NodeTestPlugin feature index out of range: ${index}`);
    },
    createFeatureInterface(index) {
      if (index !== 0n) {
        throw new Error(`NodeTestPlugin feature interface out of range: ${index}`);
      }
      return createFactory(sessionId);
    },
    canUnloadNow() {
      return true;
    },
  });

  return plugin;
}

module.exports = {
  createPlugin,
  _createComponent: createComponent,
  _createFactory: createFactory,
  _makeVariantVector: makeVariantVector,
};
