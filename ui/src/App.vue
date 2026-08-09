<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import AssistantView from '@/views/AssistantView.vue'
import ExpertView from '@/views/ExpertView.vue'
import MemoryView from '@/views/MemoryView.vue'
import ProfileView from '@/views/ProfileView.vue'
import ProcessView from '@/views/ProcessView.vue'
import SettingsView from '@/views/SettingsView.vue'

const store = useAppStore()
const activeView = ref<'assistant' | 'process' | 'memory' | 'profiles' | 'expert' | 'settings'>('assistant')

const currentView = computed(() => {
  if (activeView.value === 'process') return ProcessView
  if (activeView.value === 'memory') return MemoryView
  if (activeView.value === 'profiles') return ProfileView
  if (activeView.value === 'expert') return ExpertView
  if (activeView.value === 'settings') return SettingsView
  return AssistantView
})

onMounted(() => {
  store.init()
})
</script>

<template>
  <div class="app">
    <!-- Sidebar -->
    <aside class="sidebar">
      <div class="logo">
        <span class="logo-icon">⚡</span>
        <span class="logo-text">KillEngine</span>
      </div>

      <nav class="nav">
        <button
          class="nav-item"
          :class="{ active: activeView === 'assistant' }"
          @click="activeView = 'assistant'"
        >
          {{ $t('nav.assistant') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: activeView === 'process' }"
          @click="activeView = 'process'"
        >
          {{ $t('nav.process') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: activeView === 'memory' }"
          @click="activeView = 'memory'"
        >
          {{ $t('nav.memory') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: activeView === 'profiles' }"
          @click="activeView = 'profiles'"
        >
          {{ $t('nav.profiles') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: activeView === 'expert' }"
          @click="activeView = 'expert'"
        >
          {{ $t('nav.expert') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: activeView === 'settings' }"
          @click="activeView = 'settings'"
        >
          {{ $t('nav.settings') }}
        </button>
      </nav>

      <div class="sidebar-footer">
        <div class="credits">Pirolley Benoist</div>
        <div class="version">v{{ store.version }}</div>
        <div class="status" :class="{ connected: store.isConnected }">
          ● {{ store.statusText }}
        </div>
      </div>
    </aside>

    <!-- Main content -->
    <main class="main">
      <component :is="currentView" />
    </main>
  </div>
</template>

<style>
:root {
  --bg-primary: #1a1b26;
  --bg-secondary: #16171f;
  --bg-tertiary: #1f2031;
  --bg-accent: #2a2b3d;
  --text-primary: #c0caf5;
  --text-secondary: #7aa2f7;
  --text-dim: #565f89;
  --accent: #7aa2f7;
  --accent-hover: #89b4fa;
  --success: #9ece6a;
  --warning: #e0af68;
  --error: #f7768e;
  --border: #2a2b3d;
}

* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

html, body, #app {
  height: 100%;
  overflow: hidden;
}

body {
  font-family: 'Segoe UI', 'Cascadia Code', sans-serif;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 14px;
}

.app {
  display: flex;
  height: 100vh;
}

.sidebar {
  width: 220px;
  background: var(--bg-secondary);
  border-right: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  flex-shrink: 0;
}

.logo {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 20px 16px;
  border-bottom: 1px solid var(--border);
}

.logo-icon {
  font-size: 24px;
}

.logo-text {
  font-size: 18px;
  font-weight: 700;
  color: var(--accent);
}

.nav {
  flex: 1;
  padding: 12px 8px;
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.nav-item {
  background: transparent;
  border: none;
  color: var(--text-dim);
  padding: 10px 16px;
  text-align: left;
  border-radius: 6px;
  cursor: pointer;
  font-size: 14px;
  transition: all 0.15s;
}

.nav-item:hover:not(:disabled) {
  background: var(--bg-accent);
  color: var(--text-primary);
}

.nav-item.active {
  background: var(--bg-accent);
  color: var(--accent);
}

.nav-item:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.sidebar-footer {
  padding: 12px 16px;
  border-top: 1px solid var(--border);
}

.version {
  font-size: 11px;
  color: var(--text-dim);
  margin-bottom: 4px;
}

.credits {
  font-size: 11px;
  color: var(--text-secondary);
  margin-bottom: 4px;
}

.status {
  font-size: 12px;
  color: var(--text-dim);
}

.status.connected {
  color: var(--success);
}

.main {
  flex: 1;
  overflow-y: auto;
}
</style>
