#!/usr/bin/env node
/* The template repository's contents, made from this checkout:
 *
 *   node arcade/builder/template.mjs <out dir>
 *
 * copies arcade/template into <out dir>, and the Arcade skill into
 * .claude/skills/podbox-arcade, its paths into the SDK and manual made
 * .podbox/ ones: the template's CLAUDE.md fetches PodBox's arcade folder
 * there, so the skill's commands run as written. */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const arcade = path.resolve(here, '..');
const out = process.argv[2];
if (!out) {
    console.error('usage: node arcade/builder/template.mjs <out dir>');
    process.exit(2);
}

fs.rmSync(out, { recursive: true, force: true });
fs.cpSync(path.join(arcade, 'template'), out, { recursive: true });

let skill = fs.readFileSync(path.join(arcade, 'manual', 'skill', 'SKILL.md'), 'utf8');
skill = skill.replace(/^name: .*$/m, 'name: podbox-arcade')
             .replace(/`arcade\//g, '`.podbox/arcade/')
             .replace(/^(---\n[\s\S]*?\n---\n)/, '$1\n<!-- Text from the PodBox ' +
                      'Arcade manual, under the GPL version 2 like the rest ' +
                      'of PodBox; the template\'s LICENSE does not cover ' +
                      'this file. -->\n');
if (!skill.includes('.podbox/arcade/sdk'))
    throw new Error('template.mjs: the skill no longer names arcade/sdk');
if (!skill.includes('GPL version 2'))
    throw new Error('template.mjs: the skill has no front matter for its licence note');
const dir = path.join(out, '.claude', 'skills', 'podbox-arcade');
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(dir, 'SKILL.md'), skill);
console.log(out + ': the template, with the skill');
