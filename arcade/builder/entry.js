/* The one-file compiler's entry: the engine's .def files built in, then
 * pbgc itself, which reads them through loadDefs(). */

import opcodes from '../../apps-ipod/games/arcade/opcodes.def';
import api from '../../apps-ipod/games/arcade/arcade_api.def';

globalThis.arcadeDefs = { 'opcodes.def': opcodes, 'arcade_api.def': api };
await import('../sdk/pbgc.js');
