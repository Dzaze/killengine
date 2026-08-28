<script setup lang="ts">
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'

interface LexiconEntry {
  key: string
  term: string
  definition: string
}

const { tm, locale } = useI18n()

const search = ref('')

const allTerms = computed<LexiconEntry[]>(() => {
  const raw = tm('lexicon.terms') as Record<string, { term: string; definition: string }>
  return Object.entries(raw ?? {})
    .map(([key, value]) => ({ key, term: value.term, definition: value.definition }))
    .sort((a, b) => a.term.localeCompare(b.term, locale.value))
})

const filteredTerms = computed(() => {
  const query = search.value.trim().toLowerCase()
  if (!query) return allTerms.value
  return allTerms.value.filter(
    (entry) => entry.term.toLowerCase().includes(query) || entry.definition.toLowerCase().includes(query),
  )
})

const groupedTerms = computed(() => {
  const groups = new Map<string, LexiconEntry[]>()
  for (const entry of filteredTerms.value) {
    const letter = entry.term.charAt(0).toUpperCase()
    groups.set(letter, [...(groups.get(letter) ?? []), entry])
  }
  return Array.from(groups.entries()).map(([letter, terms]) => ({ letter, terms }))
})
</script>

<template>
  <div class="lexicon-view">
    <div class="header">
      <div>
        <h1>{{ $t('nav.lexicon') }}</h1>
        <p>Les termes techniques employés dans KillEngine, expliqués simplement.</p>
      </div>
    </div>

    <input
      v-model="search"
      type="text"
      class="search-input"
      :placeholder="$t('lexicon.searchPlaceholder')"
    />

    <div v-if="filteredTerms.length === 0" class="empty">{{ $t('lexicon.empty') }}</div>

    <div v-for="group in groupedTerms" :key="group.letter" class="letter-group">
      <div class="letter-heading">{{ group.letter }}</div>
      <div v-for="entry in group.terms" :key="entry.key" class="term-card">
        <div class="term-name">{{ entry.term }}</div>
        <div class="term-definition">{{ entry.definition }}</div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.lexicon-view {
  padding: 24px 32px;
  max-width: 800px;
}

.header {
  margin-bottom: 16px;
}

.header h1 {
  font-size: 22px;
  color: var(--text-primary);
}

.header p {
  margin-top: 4px;
  font-size: 13px;
  color: var(--text-dim);
}

.search-input {
  width: 100%;
  padding: 10px 14px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-tertiary);
  color: var(--text-primary);
  font-size: 13px;
  outline: none;
  margin-bottom: 20px;
}

.search-input:focus {
  border-color: var(--accent);
}

.empty {
  color: var(--text-dim);
  font-size: 13px;
}

.letter-group {
  margin-bottom: 18px;
}

.letter-heading {
  font-size: 13px;
  font-weight: 700;
  color: var(--accent);
  margin-bottom: 8px;
  padding-bottom: 4px;
  border-bottom: 1px solid var(--border);
}

.term-card {
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 12px 14px;
  margin-bottom: 8px;
}

.term-name {
  font-size: 14px;
  font-weight: 600;
  color: var(--text-primary);
  margin-bottom: 4px;
}

.term-definition {
  font-size: 13px;
  color: var(--text-secondary);
  line-height: 1.5;
}
</style>
