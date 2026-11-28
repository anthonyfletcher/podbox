
  var Module = typeof Module !== 'undefined' ? Module : {};

  if (!Module['expectedDataFileDownloads']) Module['expectedDataFileDownloads'] = 0;
  Module['expectedDataFileDownloads']++;
  (() => {
    // Do not attempt to redownload the virtual filesystem data when in a pthread or a Wasm Worker context.
    var isPthread = typeof ENVIRONMENT_IS_PTHREAD !== 'undefined' && ENVIRONMENT_IS_PTHREAD;
    var isWasmWorker = typeof ENVIRONMENT_IS_WASM_WORKER !== 'undefined' && ENVIRONMENT_IS_WASM_WORKER;
    if (isPthread || isWasmWorker) return;
    var isNode = globalThis.process && globalThis.process.versions && globalThis.process.versions.node && globalThis.process.type != 'renderer';
    async function loadPackage(metadata) {

      var PACKAGE_PATH = '';
      if (typeof window === 'object') {
        PACKAGE_PATH = window['encodeURIComponent'](window.location.pathname.substring(0, window.location.pathname.lastIndexOf('/')) + '/');
      } else if (typeof process === 'undefined' && typeof location !== 'undefined') {
        // web worker
        PACKAGE_PATH = encodeURIComponent(location.pathname.substring(0, location.pathname.lastIndexOf('/')) + '/');
      }
      var PACKAGE_NAME = 'rockboxui.data';
      var REMOTE_PACKAGE_BASE = 'rockboxui.data';
      var REMOTE_PACKAGE_NAME = Module['locateFile'] ? Module['locateFile'](REMOTE_PACKAGE_BASE, '') : REMOTE_PACKAGE_BASE;
      var REMOTE_PACKAGE_SIZE = metadata['remote_package_size'];

      async function fetchRemotePackage(packageName, packageSize) {
        if (isNode) {
          var contents = require('fs').readFileSync(packageName);
          return new Uint8Array(contents).buffer;
        }
        if (!Module['dataFileDownloads']) Module['dataFileDownloads'] = {};
        try {
          var response = await fetch(packageName);
        } catch (e) {
          throw new Error(`Network Error: ${packageName}`, {e});
        }
        if (!response.ok) {
          throw new Error(`${response.status}: ${response.url}`);
        }

        const chunks = [];
        const headers = response.headers;
        const total = Number(headers.get('Content-Length') || packageSize);
        let loaded = 0;

        Module['setStatus'] && Module['setStatus']('Downloading data...');
        const reader = response.body.getReader();

        while (1) {
          var {done, value} = await reader.read();
          if (done) break;
          chunks.push(value);
          loaded += value.length;
          Module['dataFileDownloads'][packageName] = {loaded, total};

          let totalLoaded = 0;
          let totalSize = 0;

          for (const download of Object.values(Module['dataFileDownloads'])) {
            totalLoaded += download.loaded;
            totalSize += download.total;
          }

          Module['setStatus'] && Module['setStatus'](`Downloading data... (${totalLoaded}/${totalSize})`);
        }

        const packageData = new Uint8Array(chunks.map((c) => c.length).reduce((a, b) => a + b, 0));
        let offset = 0;
        for (const chunk of chunks) {
          packageData.set(chunk, offset);
          offset += chunk.length;
        }
        return packageData.buffer;
      }

      var fetchPromise;
      var fetched = Module['getPreloadedPackage'] && Module['getPreloadedPackage'](REMOTE_PACKAGE_NAME, REMOTE_PACKAGE_SIZE);

      if (!fetched) {
        // Note that we don't use await here because we want to execute the
        // the rest of this function immediately.
        fetchPromise = fetchRemotePackage(REMOTE_PACKAGE_NAME, REMOTE_PACKAGE_SIZE);
      }

    async function runWithFS(Module) {

      function assert(check, msg) {
        if (!check) throw new Error(msg);
      }
Module['FS_createPath']("/", "simdisk", true, true);
Module['FS_createPath']("/simdisk", ".rockbox", true, true);
Module['FS_createPath']("/simdisk/.rockbox", "codepages", true, true);
Module['FS_createPath']("/simdisk/.rockbox", "docs", true, true);
Module['FS_createPath']("/simdisk/.rockbox", "icons", true, true);
Module['FS_createPath']("/simdisk/.rockbox", "langs", true, true);
Module['FS_createPath']("/simdisk/.rockbox", "themes", true, true);
Module['FS_createPath']("/simdisk/.rockbox", "wps", true, true);
Module['FS_createPath']("/simdisk/.rockbox/wps", "scrim", true, true);
Module['FS_createPath']("/simdisk/.rockbox/wps", "scrim_p", true, true);

      async function processPackageData(arrayBuffer) {
        assert(arrayBuffer, 'Loading data file failed.');
        assert(arrayBuffer.constructor.name === ArrayBuffer.name, 'bad input to processPackageData ' + arrayBuffer.constructor.name);
        var byteArray = new Uint8Array(arrayBuffer);
        var curr;
        // Reuse the bytearray from the XHR as the source for file reads.
          for (var file of metadata['files']) {
            var name = file['filename'];
            var data = byteArray.subarray(file['start'], file['end']);
            // canOwn this data in the filesystem, it is a slice into the heap that will never change
        Module['FS_createDataFile'](name, null, data, true, true, true);
          }
          Module['removeRunDependency']('datafile_rockboxui.data');
      }
      Module['addRunDependency']('datafile_rockboxui.data');

      if (!Module['preloadResults']) Module['preloadResults'] = {};

      Module['preloadResults'][PACKAGE_NAME] = {fromCache: false};
      if (!fetched) {
        fetched = await fetchPromise;
      }
      await processPackageData(fetched);

    }
    // Detect whether the module JS file has already been loaded.
    if (Module['FS_createPath']) {
      runWithFS(Module);
    } else {
      if (!Module['preRun']) Module['preRun'] = [];
      Module['preRun'].push(runWithFS); // FS is not initialized yet, wait for it
    }

    }
    loadPackage({"files": [{"filename": "/simdisk/.rockbox/bookmark.ignore", "start": 0, "end": 0}, {"filename": "/simdisk/.rockbox/codepages/932.cp", "start": 0, "end": 65536}, {"filename": "/simdisk/.rockbox/codepages/936.cp", "start": 65536, "end": 131072}, {"filename": "/simdisk/.rockbox/codepages/949.cp", "start": 131072, "end": 196608}, {"filename": "/simdisk/.rockbox/codepages/950.cp", "start": 196608, "end": 262144}, {"filename": "/simdisk/.rockbox/codepages/iso.cp", "start": 262144, "end": 264448}, {"filename": "/simdisk/.rockbox/database.ignore", "start": 264448, "end": 264448}, {"filename": "/simdisk/.rockbox/default-config.cfg", "start": 264448, "end": 265320}, {"filename": "/simdisk/.rockbox/docs/COPYING.txt", "start": 265320, "end": 280447}, {"filename": "/simdisk/.rockbox/docs/LICENSES.txt", "start": 280447, "end": 294628}, {"filename": "/simdisk/.rockbox/docs/sample.colours", "start": 294628, "end": 294710}, {"filename": "/simdisk/.rockbox/docs/sample.icons", "start": 294710, "end": 294939}, {"filename": "/simdisk/.rockbox/docs/settings-help.txt", "start": 294939, "end": 383464}, {"filename": "/simdisk/.rockbox/icons/tango_icons.16x16.bmp", "start": 383464, "end": 416354}, {"filename": "/simdisk/.rockbox/icons/tango_icons_viewers.16x16.bmp", "start": 416354, "end": 427740}, {"filename": "/simdisk/.rockbox/icons/viewers.6x8x16.bmp", "start": 427740, "end": 429874}, {"filename": "/simdisk/.rockbox/langs/InvalidVoice_english.talk", "start": 429874, "end": 433990}, {"filename": "/simdisk/.rockbox/langs/arabic.lng", "start": 433990, "end": 464328}, {"filename": "/simdisk/.rockbox/langs/basque.lng", "start": 464328, "end": 492913}, {"filename": "/simdisk/.rockbox/langs/bulgarian.lng", "start": 492913, "end": 536686}, {"filename": "/simdisk/.rockbox/langs/catala.lng", "start": 536686, "end": 566121}, {"filename": "/simdisk/.rockbox/langs/chinese-simp.lng", "start": 566121, "end": 594405}, {"filename": "/simdisk/.rockbox/langs/chinese-trad.lng", "start": 594405, "end": 622714}, {"filename": "/simdisk/.rockbox/langs/czech.lng", "start": 622714, "end": 652915}, {"filename": "/simdisk/.rockbox/langs/dansk.lng", "start": 652915, "end": 681172}, {"filename": "/simdisk/.rockbox/langs/deutsch.lng", "start": 681172, "end": 713582}, {"filename": "/simdisk/.rockbox/langs/eesti.lng", "start": 713582, "end": 741650}, {"filename": "/simdisk/.rockbox/langs/english-us.lng", "start": 741650, "end": 768976}, {"filename": "/simdisk/.rockbox/langs/english.lng", "start": 768976, "end": 796348}, {"filename": "/simdisk/.rockbox/langs/espanol.lng", "start": 796348, "end": 831202}, {"filename": "/simdisk/.rockbox/langs/esperanto.lng", "start": 831202, "end": 859301}, {"filename": "/simdisk/.rockbox/langs/finnish.lng", "start": 859301, "end": 888347}, {"filename": "/simdisk/.rockbox/langs/francais.lng", "start": 888347, "end": 923578}, {"filename": "/simdisk/.rockbox/langs/galego.lng", "start": 923578, "end": 953083}, {"filename": "/simdisk/.rockbox/langs/greek.lng", "start": 953083, "end": 993464}, {"filename": "/simdisk/.rockbox/langs/hebrew.lng", "start": 993464, "end": 1024913}, {"filename": "/simdisk/.rockbox/langs/hindi.lng", "start": 1024913, "end": 1055555}, {"filename": "/simdisk/.rockbox/langs/hrvatski.lng", "start": 1055555, "end": 1085136}, {"filename": "/simdisk/.rockbox/langs/islenska.lng", "start": 1085136, "end": 1113087}, {"filename": "/simdisk/.rockbox/langs/italiano.lng", "start": 1113087, "end": 1145385}, {"filename": "/simdisk/.rockbox/langs/japanese.lng", "start": 1145385, "end": 1185887}, {"filename": "/simdisk/.rockbox/langs/korean.lng", "start": 1185887, "end": 1216481}, {"filename": "/simdisk/.rockbox/langs/latviesu.lng", "start": 1216481, "end": 1248515}, {"filename": "/simdisk/.rockbox/langs/lietuviu.lng", "start": 1248515, "end": 1277683}, {"filename": "/simdisk/.rockbox/langs/magyar.lng", "start": 1277683, "end": 1309979}, {"filename": "/simdisk/.rockbox/langs/moldoveneste.lng", "start": 1309979, "end": 1340568}, {"filename": "/simdisk/.rockbox/langs/nederlands.lng", "start": 1340568, "end": 1372328}, {"filename": "/simdisk/.rockbox/langs/norsk-nynorsk.lng", "start": 1372328, "end": 1400360}, {"filename": "/simdisk/.rockbox/langs/norsk.lng", "start": 1400360, "end": 1428638}, {"filename": "/simdisk/.rockbox/langs/polski.lng", "start": 1428638, "end": 1462507}, {"filename": "/simdisk/.rockbox/langs/portugues-brasileiro.lng", "start": 1462507, "end": 1495824}, {"filename": "/simdisk/.rockbox/langs/portugues.lng", "start": 1495824, "end": 1525356}, {"filename": "/simdisk/.rockbox/langs/romaneste.lng", "start": 1525356, "end": 1555944}, {"filename": "/simdisk/.rockbox/langs/russian.lng", "start": 1555944, "end": 1598320}, {"filename": "/simdisk/.rockbox/langs/slovak.lng", "start": 1598320, "end": 1629261}, {"filename": "/simdisk/.rockbox/langs/slovenscina.lng", "start": 1629261, "end": 1657283}, {"filename": "/simdisk/.rockbox/langs/srpski.lng", "start": 1657283, "end": 1698170}, {"filename": "/simdisk/.rockbox/langs/svenska.lng", "start": 1698170, "end": 1728090}, {"filename": "/simdisk/.rockbox/langs/tagalog.lng", "start": 1728090, "end": 1756278}, {"filename": "/simdisk/.rockbox/langs/thai.lng", "start": 1756278, "end": 1795895}, {"filename": "/simdisk/.rockbox/langs/turkce.lng", "start": 1795895, "end": 1826680}, {"filename": "/simdisk/.rockbox/langs/ukrainian.lng", "start": 1826680, "end": 1868823}, {"filename": "/simdisk/.rockbox/langs/vietnamese.lng", "start": 1868823, "end": 1902276}, {"filename": "/simdisk/.rockbox/langs/vlaams.lng", "start": 1902276, "end": 1931405}, {"filename": "/simdisk/.rockbox/langs/voicestrings.zip", "start": 1931405, "end": 2664392}, {"filename": "/simdisk/.rockbox/langs/walon.lng", "start": 2664392, "end": 2693990}, {"filename": "/simdisk/.rockbox/rockbox-info.txt", "start": 2693990, "end": 2694971}, {"filename": "/simdisk/.rockbox/tagnavi.config", "start": 2694971, "end": 2704099}, {"filename": "/simdisk/.rockbox/talkclips.ignore", "start": 2704099, "end": 2704099}, {"filename": "/simdisk/.rockbox/themes/rockbox_default_icons.cfg", "start": 2704099, "end": 2704241}, {"filename": "/simdisk/.rockbox/themes/rockbox_failsafe.cfg", "start": 2704241, "end": 2704617}, {"filename": "/simdisk/.rockbox/themes/scrim.cfg", "start": 2704617, "end": 2705261}, {"filename": "/simdisk/.rockbox/themes/scrim.options", "start": 2705261, "end": 2707107}, {"filename": "/simdisk/.rockbox/themes/scrim.preferences", "start": 2707107, "end": 2708778}, {"filename": "/simdisk/.rockbox/themes/scrim_p.cfg", "start": 2708778, "end": 2709426}, {"filename": "/simdisk/.rockbox/trim.config", "start": 2709426, "end": 2711452}, {"filename": "/simdisk/.rockbox/wps/rockbox_failsafe.fms", "start": 2711452, "end": 2711584}, {"filename": "/simdisk/.rockbox/wps/rockbox_failsafe.sbs", "start": 2711584, "end": 2711716}, {"filename": "/simdisk/.rockbox/wps/rockbox_failsafe.wps", "start": 2711716, "end": 2711848}, {"filename": "/simdisk/.rockbox/wps/rockbox_failsafe.wrs", "start": 2711848, "end": 2711980}, {"filename": "/simdisk/.rockbox/wps/scrim.sbs", "start": 2711980, "end": 2731269}, {"filename": "/simdisk/.rockbox/wps/scrim.wps", "start": 2731269, "end": 2734784}, {"filename": "/simdisk/.rockbox/wps/scrim/volband.bmp", "start": 2734784, "end": 2739158}, {"filename": "/simdisk/.rockbox/wps/scrim/volfill.bmp", "start": 2739158, "end": 2743532}, {"filename": "/simdisk/.rockbox/wps/scrim_p.wps", "start": 2743532, "end": 2746152}, {"filename": "/simdisk/.rockbox/wps/scrim_p/volband.bmp", "start": 2746152, "end": 2750526}, {"filename": "/simdisk/.rockbox/wps/scrim_p/volfill.bmp", "start": 2750526, "end": 2754900}], "remote_package_size": 2754900});

  })();
