'use strict';

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
    throw new Error(`Unknown DAS node host argument: ${arg}`);
  }

  if (!options.mainPid && !options.connectUrl) {
    throw new Error('Either --main-pid or --connect-url must be specified');
  }

  return options;
}

function loadNativeBootstrap() {
  const das = require(path.join(__dirname, 'das_core_napi_export.js'));
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

  const das = loadNativeBootstrap();

  const result = await das.startHostIpc({
    mainPid: options.mainPid,
    connectUrl: options.connectUrl,
    packageRoot: __dirname,
    wrapperPath: path.join(__dirname, 'das_core_napi_export.js'),
    addonPath: path.join(__dirname, 'das_core_napi.node'),
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
  loadNativeBootstrap,
  main,
  parseArgs,
};
