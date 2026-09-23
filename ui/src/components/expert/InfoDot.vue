<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'

const props = defineProps<{
  /** Cle i18n sous `help.` — ex. `topic="scanExact"` lit `help.scanExact.*`. */
  topic?: string
  /** Texte libre, utilise quand aucun `topic` n'est fourni. */
  text?: string
  /** Aligne le popover a droite quand la pastille est en fin de ligne. */
  align?: 'left' | 'right'
}>()

const { t, te } = useI18n()
const open = ref(false)
const root = ref<HTMLElement | null>(null)

// `te()` evite d'afficher la cle brute quand une traduction manque.
const key = (suffix: string) => `help.${props.topic}.${suffix}`
const has = (suffix: string) => Boolean(props.topic) && te(key(suffix))
const value = (suffix: string) => t(key(suffix))

const title = computed(() => (has('title') ? value('title') : ''))
const rows = computed(() =>
  (['what', 'when', 'cost'] as const)
    .filter((suffix) => has(suffix))
    .map((suffix) => ({ suffix, label: t(`help.label.${suffix}`), body: value(suffix) })),
)
const example = computed(() => (has('example') ? value('example') : ''))
const freeText = computed(() => (rows.value.length === 0 ? props.text ?? '' : ''))
const label = computed(() => (title.value ? `${t('help.label.about')} ${title.value}` : t('help.label.about')))

function toggle() {
  open.value = !open.value
}

function onDocumentPointerDown(event: MouseEvent) {
  if (!open.value) return
  if (root.value && !root.value.contains(event.target as Node)) open.value = false
}

function onDocumentKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') open.value = false
}

onMounted(() => {
  document.addEventListener('mousedown', onDocumentPointerDown)
  document.addEventListener('keydown', onDocumentKeydown)
})

onBeforeUnmount(() => {
  document.removeEventListener('mousedown', onDocumentPointerDown)
  document.removeEventListener('keydown', onDocumentKeydown)
})
</script>

<template>
  <span ref="root" class="info-dot">
    <button
      type="button"
      class="info-dot-trigger"
      :class="{ open }"
      :aria-label="label"
      :aria-expanded="open"
      @click.stop="toggle()"
    >
      ?
    </button>
    <div v-if="open" class="info-dot-popover" :class="align ?? 'left'" role="dialog" :aria-label="label">
      <strong v-if="title" class="info-dot-title">{{ title }}</strong>
      <p v-if="freeText" class="info-dot-free">{{ freeText }}</p>
      <dl v-else class="info-dot-rows">
        <template v-for="row in rows" :key="row.suffix">
          <dt :class="row.suffix">{{ row.label }}</dt>
          <dd>{{ row.body }}</dd>
        </template>
      </dl>
      <div v-if="example" class="info-dot-example">
        <span class="info-dot-example-label">{{ t('help.label.example') }}</span>
        <p>{{ example }}</p>
      </div>
      <slot />
    </div>
  </span>
</template>

<style scoped>
.info-dot {
  position: relative;
  display: inline-flex;
  vertical-align: middle;
}

.info-dot-trigger {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 16px;
  height: 16px;
  padding: 0;
  border: 1px solid var(--border);
  border-radius: 50%;
  background: transparent;
  color: var(--text-muted);
  font-size: 11px;
  font-weight: 700;
  line-height: 1;
  cursor: pointer;
  transition: color 0.12s, border-color 0.12s;
}

.info-dot-trigger:hover,
.info-dot-trigger:focus-visible,
.info-dot-trigger.open {
  border-color: var(--accent);
  color: var(--accent);
  outline: none;
}

.info-dot-popover {
  position: absolute;
  top: 22px;
  z-index: 40;
  width: 320px;
  padding: 10px 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.45);
  text-align: left;
  white-space: normal;
  cursor: default;
}

.info-dot-popover.left {
  left: 0;
}

.info-dot-popover.right {
  right: 0;
}

.info-dot-title {
  display: block;
  margin-bottom: 6px;
  color: var(--text-primary);
  font-size: 12px;
}

.info-dot-free {
  margin: 0;
  color: var(--text-muted);
  font-size: 12px;
  line-height: 1.5;
}

.info-dot-rows {
  display: grid;
  grid-template-columns: auto 1fr;
  gap: 3px 8px;
  margin: 0;
}

.info-dot-rows dt {
  color: var(--text-secondary);
  font-size: 11px;
  font-weight: 600;
  white-space: nowrap;
}

.info-dot-rows dt.cost {
  color: var(--warning);
}

.info-dot-rows dd {
  margin: 0;
  color: var(--text-muted);
  font-size: 12px;
  line-height: 1.45;
}

.info-dot-example {
  margin-top: 8px;
  padding: 7px 9px;
  border: 1px solid rgba(122, 162, 247, 0.25);
  border-left: 2px solid var(--accent);
  border-radius: 4px;
  background: var(--bg-primary);
}

.info-dot-example-label {
  display: block;
  margin-bottom: 3px;
  color: var(--accent-hover);
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.03em;
  text-transform: uppercase;
}

.info-dot-example p {
  margin: 0;
  color: var(--text-muted);
  font-size: 12px;
  line-height: 1.5;
}
</style>
