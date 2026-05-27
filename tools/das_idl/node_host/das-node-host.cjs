'use strict';

const fs = require('node:fs');
const path = require('node:path');

function readValue(rawArgs, index, optionName) {
  const value = rawArgs[index + 1];
  if (!value || value.startsWith('--')) {
    throw new Error(`${optionName} requires a value`);
  }
  return value;
}

function parsePositiveInteger(value, optionName) {
  if (!/^[0-9]+$/.test(value)) {
    throw new Error(`${optionName} must be a positive integer`);
  }
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed <= 0) {
    throw new Error(`${optionName} must be a positive integer`);
  }
  return parsed;
}

function parseArgs(rawArgs = process.argv.slice(2)) {
  const options = {
    dryRunParse: false,
    mainPid: undefined,
    connectUrl: undefined,
    packageRoot: undefined,
    nodeModulesRoot: undefined,
  };

  for (let index = 0; index < rawArgs.length; index += 1) {
    const arg = rawArgs[index];
    if (arg === '--dry-run-parse') {
      options.dryRunParse = true;
      continue;
    }
    if (arg === '--main-pid') {
      const value = readValue(rawArgs, index, arg);
      options.mainPid = parsePositiveInteger(value, arg);
      index += 1;
      continue;
    }
    if (arg === '--connect-url') {
      options.connectUrl = readValue(rawArgs, index, arg);
      index += 1;
      continue;
    }
    if (arg === '--package-root') {
      options.packageRoot = path.resolve(readValue(rawArgs, index, arg));
      index += 1;
      continue;
    }
    if (arg === '--node-modules-root') {
      options.nodeModulesRoot = path.resolve(readValue(rawArgs, index, arg));
      index += 1;
      continue;
    }
    throw new Error(`Unknown DAS node host argument: ${arg}`);
  }

  if (!options.mainPid && !options.connectUrl) {
    throw new Error('Either --main-pid or --connect-url must be specified');
  }
  if (!options.packageRoot) {
    throw new Error('--package-root must be specified');
  }
  if (!options.nodeModulesRoot) {
    throw new Error('--node-modules-root must be specified');
  }

  return options;
}

function getDynamicLibraryPathEnvName() {
  if (process.platform === 'win32') {
    return 'PATH';
  }
  if (process.platform === 'darwin') {
    return 'DYLD_LIBRARY_PATH';
  }
  return 'LD_LIBRARY_PATH';
}

function samePath(left, right) {
  const normalizedLeft = path.resolve(left);
  const normalizedRight = path.resolve(right);
  if (process.platform === 'win32') {
    return normalizedLeft.toLowerCase() === normalizedRight.toLowerCase();
  }
  return normalizedLeft === normalizedRight;
}

function findPackageRoot(resolvedEntry) {
  let current = path.dirname(resolvedEntry);
  while (current && current !== path.dirname(current)) {
    if (fs.existsSync(path.join(current, 'package.json'))) {
      return current;
    }
    current = path.dirname(current);
  }
  return path.dirname(resolvedEntry);
}

function printDiagnostics(options, runtimeRoot) {
  const envName = getDynamicLibraryPathEnvName();
  console.error('[DAS node host] diagnostics');
  console.error(`[DAS node host] platform=${process.platform}`);
  console.error(`[DAS node host] execPath=${process.execPath}`);
  console.error(`[DAS node host] cwd=${process.cwd()}`);
  console.error(`[DAS node host] hostScriptDir=${__dirname}`);
  console.error(`[DAS node host] runtimeRoot=${runtimeRoot}`);
  console.error(`[DAS node host] packageRoot=${options.packageRoot}`);
  console.error(`[DAS node host] nodeModulesRoot=${options.nodeModulesRoot}`);
  console.error(`[DAS node host] ${envName}=${process.env[envName] || ''}`);
}

function warnIfPluginShadowsRuntime(options, runtimeRoot) {
  let resolvedEntry;
  try {
    resolvedEntry = require.resolve('das-core-node', {
      paths: [options.packageRoot],
    });
  } catch (error) {
    console.warn(
      `[DAS node host] unable to resolve das-core-node from plugin package root: ${error.message}`,
    );
    return;
  }

  const resolvedRoot = findPackageRoot(resolvedEntry);
  if (!samePath(resolvedRoot, runtimeRoot)) {
    console.warn(
      `[DAS node host] plugin-local das-core-node at ${resolvedRoot} shadows DAS runtime package ${runtimeRoot}`,
    );
  }
}

function loadNativeBootstrap(runtimeRoot) {
  const das = require(path.join(runtimeRoot, 'index.cjs'));
  if (typeof das.startHostIpc !== 'function') {
    throw new Error('DAS native addon does not export startHostIpc');
  }
  return das;
}

async function main(rawArgs = process.argv.slice(2)) {
  const options = parseArgs(rawArgs);
  if (options.dryRunParse) {
    return 0;
  }

  const runtimeRoot = path.resolve(__dirname, '..');
  printDiagnostics(options, runtimeRoot);
  warnIfPluginShadowsRuntime(options, runtimeRoot);

  const das = loadNativeBootstrap(runtimeRoot);

  const result = await das.startHostIpc({
    mainPid: options.mainPid,
    connectUrl: options.connectUrl,
    packageRoot: options.packageRoot,
    nodeModulesRoot: options.nodeModulesRoot,
    wrapperPath: path.join(runtimeRoot, 'das_core_napi_export.js'),
    addonPath: path.join(runtimeRoot, 'native', 'das_core_napi.node'),
    requireFunction: require,
  });

  return typeof result === 'number' ? result : 0;
}

if (require.main === module) {
  main()
    .then((result) => {
      process.exitCode = result === 0 ? 0 : 1;
    })
    .catch((error) => {
      console.error(error && error.stack ? error.stack : String(error));
      process.exitCode = 1;
    });
}

module.exports = {
  findPackageRoot,
  getDynamicLibraryPathEnvName,
  loadNativeBootstrap,
  main,
  parseArgs,
  warnIfPluginShadowsRuntime,
};
