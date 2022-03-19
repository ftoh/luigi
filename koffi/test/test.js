#!/usr/bin/env node

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

'use strict';

const process = require('process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const util = require('util');
const { spawn, spawnSync } = require('child_process');
const { NodeSSH } = require('node-ssh');
const chalk = require('chalk');

// Globals

let script_dir = null;
let root_dir = null;

let machines = [];
let display = false;
let accelerate = true;
let ignore = new Set;

// Main

main();

async function main() {
    script_dir = fs.realpathSync(path.dirname(__filename));
    root_dir = fs.realpathSync(script_dir + '/../..');

    // All the code assumes we are working from the script directory
    process.chdir(script_dir);

    let command = test;

    // Parse options
    {
        let i = 2;

        if (process.argv.length >= 3 && process.argv[2][0] != '-') {
            switch (process.argv[2]) {
                case 'test': { command = test; } break;
                case 'start': { command = start; } break;
                case 'stop': { command = stop; } break;
                case 'ssh': { command = ssh; } break;
                case 'reset': { command = reset; } break;

                default: {
                    throw new Error(`Unknown command '${process.argv[2]}'`);
                } break;
            }

            i++;
        }

        for (; i < process.argv.length; i++) {
            let arg = process.argv[i];
            let value = null;

            if (arg[0] == '-') {
                if (arg.length > 2 && arg[1] != '-') {
                    value = arg.substr(2);
                    arg = arg.substr(0, 2);
                } else if (arg[1] == '-') {
                    let offset = arg.indexOf('=');

                    if (offset > 2 && arg.length > offset + 1) {
                        value = arg.substr(offset + 1);
                        arg = arg.substr(0, offset);
                    }
                }
                if (value == null && process.argv[i + 1] != null && process.argv[i + 1][0] != '-') {
                    value = process.argv[i + 1];
                    i++; // Skip this value next iteration
                }
            }

            if (arg == '--help') {
                print_usage();
                return;
            } else if ((command == test || command == start) && (arg == '-d' || arg == '--display')) {
                display = true;
            } else if ((command == test || command == start) && arg == '--no-accel') {
                accelerate = false;
            } else if (arg[0] == '-') {
                throw new Error(`Unexpected argument '${arg}'`);
            } else {
                if (arg.startsWith('__') || arg.match(/[\\/\.]/))
                    throw new Error(`Machine name '${arg} is not valid`);

                machines.push(arg);
            }
        }
    }

    // Load machine registry
    let machines_map;
    {
        let json = fs.readFileSync('registry/machines.json', { encoding: 'utf-8' });

        machines_map = JSON.parse(json);
        for (let key in machines_map) {
            let machine = machines_map[key];

            machine.key = key;
            machine.started = false;

            machine.qemu.accelerate = null;
            if (accelerate) {
                if (process.arch == 'x64' && (machine.info.arch == 'x64' || machine.info.arch == 'ia32')) {
                    switch (process.platform) {
                        case 'linux': { machine.qemu.accelerate = 'kvm'; } break;
                        case 'win32': { machine.qemu.accelerate = 'whpx'; } break;
                    }
                }
            }
        }
    }

    if (!machines.length) {
        for (let name in machines_map)
            machines.push(name);

        if (!machines.length) {
            console.error('Could not detect any machine');
            process.exit(1);
        }
    }
    machines = machines.map(name => {
        let machine = machines_map[name];
        if (machine == null) {
            machine = Object.values(machines_map).find(machine => machine.name == name);
            if (machine == null)
                throw new Error(`Could not find machine ${name}`);
        }
        return machine;
    });

    try {
        let success = await command();
        process.exit(!success);
    } catch (err) {
        console.error(err);
        process.exit(1);
    }
}

function print_usage() {
    let help = `Usage: node test.js [command] [options...]

Commands:
    test                         Run the machines and perform the tests (default)
    start                        Start the machines but don't run anythingh
    stop                         Stop running machines
    ssh                          Start SSH session with specific machine
    reset                        Reset initial disk snapshot

Options:
    -d, --display                Show the QEMU display during the procedure
`;

    console.log(help);
}

// Commands

async function start(detach = true) {
    let success = true;
    let missing = 0;

    console.log('>> Starting up machines...');
    await Promise.all(machines.map(async machine => {
        let dirname = `qemu/${machine.key}`;

        if (!fs.existsSync(dirname)) {
            log(machine, 'Missing files', chalk.bold.gray('[ignore]'));

            ignore.add(machine);
            missing++;

            return;
        }

        // Version check
        {
            let filename = dirname + '/VERSION';
            let version = fs.existsSync(filename) ? parseInt(fs.readFileSync(filename).toString(), 10) : 0;

            if (version != machine.info.version) {
                log(machine, 'Download newer machine', chalk.bold.gray('[ignore]'));

                ignore.add(machine);
                missing++;

                return;
            }
        }

        try {
            await boot(machine, dirname, detach, display);

            if (machine.started) {
                let action = `Start (${machine.qemu.accelerate || 'emulated'})`;
                log(machine, action, chalk.bold.green('[ok]'));
            } else {
                log(machine, 'Join', chalk.bold.green('[ok]'));
            }
        } catch (err) {
            log(machine, 'Start', chalk.bold.red('[error]'));

            ignore.add(machine);
            missing++;
        }
    }));

    if (success && missing == machines.length)
        throw new Error('No machine available');

    return success;
}

async function test() {
    let success = true;

    let snapshot_dir = fs.mkdtempSync(path.join(os.tmpdir(), 'luigi-'));
    process.on('exit', () => unlink_recursive(snapshot_dir));

    console.log('>> Snapshot code...');
    copy_recursive(root_dir, snapshot_dir, filename => {
        let basename = path.basename(filename);

        return basename !== '.git' &&
               basename !== 'qemu' && !basename.startsWith('qemu_') &&
               basename !== 'node_modules' &&
               basename !== 'node' &&
               basename !== 'build' &&
               basename !== 'luigi';
    });

    success &= await start(false);

    console.log('>> Copy source code...');
    await Promise.all(machines.map(async machine => {
        if (ignore.has(machine))
            return;

        let copied = true;

        for (let test of Object.values(machine.tests)) {
            try {
                await machine.ssh.exec('rm', ['-rf', test.directory]);
            } catch (err) {
                // Fails often on Windows (busy directory or whatever), but rarely a problem
            }

            try {
                await machine.ssh.putDirectory(snapshot_dir, test.directory, {
                    recursive: true,
                    concurrency: 4
                });
            } catch (err) {
                ignore.add(machine);
                success = false;
                copied = false;
            }
        }

        let status = copied ? chalk.bold.green('[ok]') : chalk.bold.red('[error]');
        log(machine, 'Copy', status);
    }));

    console.log('>> Run test commands...');
    await Promise.all(machines.map(async machine => {
        if (ignore.has(machine))
            return;

        await Promise.all(Object.keys(machine.tests).map(async suite => {
            let test = machine.tests[suite];
            let commands = {
                Build: test.build,
                ...test.commands
            };

            for (let name in commands) {
                let cmd = commands[name];
                let cwd = test.directory + '/koffi';

                let start = process.hrtime.bigint();
                let ret = await exec_remote(machine, cmd, cwd);
                let time = Number((process.hrtime.bigint() - start) / 1000000n);

                if (ret.code == 0) {
                    log(machine, `${suite} > ${name}`, chalk.bold.green(`[${(time / 1000).toFixed(2)}s]`));
                } else {
                    log(machine, `${suite} > ${name}`, chalk.bold.red('[error]'));

                    if (ret.stdout || ret.stderr)
                        console.error('');

                    let align = log.align + 9;
                    if (ret.stdout) {
                        let str = ' '.repeat(align) + 'Standard output:\n' +
                                  chalk.yellow(ret.stdout.replace(/^/gm, ' '.repeat(align + 4))) + '\n';
                        console.error(str);
                    }
                    if (ret.stderr) {
                        let str = ' '.repeat(align) + 'Standard error:\n' +
                                  chalk.yellow(ret.stderr.replace(/^/gm, ' '.repeat(align + 4))) + '\n';
                        console.error(str);
                    }

                    success = false;

                    if (name == 'Build')
                        break;
                }
            }
        }));
    }));

    if (machines.some(machine => machine.started))
        success &= await stop(false);

    console.log('');
    if (success) {
        console.log('>> Status: ' + chalk.bold.green('SUCCESS'));
        if (ignore.size)
            console.log('   (but some machines could not be tested)');
    } else {
        console.log('>> Status: ' + chalk.bold.red('FAILED'));
    }

    return success;
}

async function stop(all = true) {
    let success = true;

    console.log('>> Sending shutdown commands...');
    await Promise.all(machines.map(async machine => {
        if (ignore.has(machine))
            return;
        if (!machine.started && !all)
            return;

        if (machine.ssh == null) {
            try {
                await join(machine, 2);
            } catch (err) {
                log(machine, 'Already down', chalk.bold.green('[ok]'));
                return;
            }
        }

        try {
            await new Promise(async (resolve, reject) => {
                if (machine.ssh.connection == null) {
                    reject();
                    return;
                }

                machine.ssh.connection.on('close', resolve);
                machine.ssh.connection.on('end', resolve);
                wait(60000).then(() => { reject(new Error('Timeout')) });

                exec_remote(machine, machine.info.shutdown);
            });

            log(machine, 'Stop', chalk.bold.green('[ok]'));
        } catch (err) {
            log(machine, 'Stop', chalk.bold.red('[error]'));
            success = false;
        }
    }));

    return success;
}

async function ssh() {
    if (machines.length != 1) {
        console.error('The ssh command can only be used with one machine');
        return false;
    }

    let machine = machines[0];

    let args = [
        '-p' + machine.info.password,
        'ssh', '-o', 'StrictHostKeyChecking=no',
               '-o', 'UserKnownHostsFile=' + (process.platform == 'win32' ? 'NUL' : '/dev/null'),
               '-p', machine.info.port, machine.info.username + '@127.0.0.1'
    ];

    let proc = spawnSync('sshpass', args, { stdio: 'inherit' });
    if (proc.status != 0) {
        console.error('Connection failed');
        return false;
    }

    return true;
}

async function reset() {
    console.log('>> Restoring snapshots...')
    await Promise.all(machines.map(machine => {
        let dirname = `qemu/${machine.key}`;
        let disk = dirname + '/disk.qcow2';

        let proc = spawnSync('qemu-img', ['snapshot', disk, '-a', 'base']);

        if (!proc.status) {
            log(machine, 'Reset disk', chalk.bold.green('[ok]'));
        } else {
            log(machine, 'Reset disk', chalk.bold.red('[error]'));

            if (proc.stderr) {
                console.error('');

                let align = log.align + 9;
                let str = ' '.repeat(align) + 'Standard error:\n' +
                          chalk.yellow(ret.stderr.replace(/^/gm, ' '.repeat(align + 4))) + '\n';
                console.error(str);
            }
        }
    }));
}

// Utility

function copy_recursive(src, dest, validate = filename => true) {
    let entries = fs.readdirSync(src, { withFileTypes: true });

    for (let entry of entries) {
        let filename = path.join(src, entry.name);
        let destname = path.join(dest, entry.name);

        if (!validate(filename))
            continue;

        if (entry.isDirectory()) {
            fs.mkdirSync(destname, { mode: 0o755 });
            copy_recursive(filename, destname, validate);
        } else if (entry.isFile()) {
            fs.copyFileSync(filename, destname);
        }
    }
}

function unlink_recursive(path) {
    try {
        if (fs.rmSync != null) {
            fs.rmSync(path, { recursive: true, maxRetries: process.platform == 'win32' ? 3 : 0 });
        } else {
            fs.rmdirSync(path, { recursive: true, maxRetries: process.platform == 'win32' ? 3 : 0 });
        }
    } catch (err) {
        if (err.code !== 'ENOENT')
            throw err;
    }
}

async function boot(machine, dirname, detach, display) {
    let args = machine.qemu.arguments.slice();

    if (machine.qemu.accelerate)
        args.push('-accel', machine.qemu.accelerate);
    if (!display)
        args.push('-display', 'none');

    try {
        let proc = spawn(machine.qemu.binary, args, {
            cwd: dirname,
            detached: detach,
            stdio: 'ignore'
        });
        if (detach)
            proc.unref();

        await new Promise((resolve, reject) => {
            proc.on('spawn', () => wait(2 * 1000).then(resolve));
            proc.on('error', reject);
            proc.on('exit', reject);
        });

        await join(machine, 30);
        machine.started = true;
    } catch (err) {
        if (typeof err != 'number')
            throw err;

        await join(machine, 2);
        machine.started = false;
    }

    if (machine.uploads != null) {
        for (let src in machine.uploads) {
            let dest = machine.uploads[src];
            await machine.ssh.putFile('files/' + src, dest);
        }
    }
}

async function join(machine, tries) {
    let ssh = new NodeSSH;

    while (tries) {
        try {
            await ssh.connect({
                host: '127.0.0.1',
                port: machine.info.port,
                username: machine.info.username,
                password: machine.info.password
            });

            break;
        } catch (err) {
            if (!--tries)
                throw new Error(`Failed to connect to ${machine.name}`);

            // Try again... a few times
            await wait(10 * 1000);
        }
    }

    machine.ssh = ssh;
}

function wait(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

function log(machine, action, status) {
    if (log.align == null) {
        let lengths = machines.map(machine => machine.name.length);
        log.align = Math.max(...lengths);
    }

    let align1 = Math.max(log.align - machine.name.length, 0);
    let align2 = Math.max(30 - action.length, 0);

    console.log(`     [${machine.name}]${' '.repeat(align1)}  ${action}${' '.repeat(align2)}  ${status}`);
}

async function exec_remote(machine, cmd, cwd = null) {
    try {
        if (machine.info.platform == 'win32') {
            if (cwd != null) {
                cwd = cwd.replaceAll('/', '\\');
                cmd = `cd "${cwd}" && ${cmd}`;
            }

            let ret = await machine.ssh.execCommand(cmd);
            return ret;
        } else {
            let ret = await machine.ssh.execCommand(cmd, { cwd: cwd });
            return ret;
        }
    } catch (err) {
        console.log(err);
        return err;
    }
}
