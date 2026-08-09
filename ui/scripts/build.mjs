import { spawnSync } from 'node:child_process'

function run(command, args) {
  const result = spawnSync(command, args, {
    shell: process.platform === 'win32',
    stdio: 'inherit',
  })

  if (result.status !== 0) {
    process.exit(result.status ?? 1)
  }
}

run('node', ['./node_modules/vue-tsc/bin/vue-tsc.js', '--noEmit'])
run('node', ['./node_modules/vite/bin/vite.js', 'build'])
