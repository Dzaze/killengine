<script setup lang="ts">
// Bloc d'introduction débutant affiché en haut de certains panneaux
// (PHASE 171 / MVP PHASE 173) : trois réponses courtes et toujours visibles
// — "C'est quoi", "À quoi ça sert", "Comment l'utiliser" — sans interaction.
// Complémentaire des InfoDot (aide contextuelle par contrôle) : ne les
// remplace pas, ne les utilise pas.
defineProps<{
  what: string
  purpose: string
  how: string
}>()
</script>

<template>
  <section class="panel-intro" :aria-label="$t('common.panelIntro.ariaLabel')">
    <div class="intro-item">
      <span class="intro-label">{{ $t('common.panelIntro.what') }}</span>
      <p>{{ what }}</p>
    </div>
    <div class="intro-item">
      <span class="intro-label">{{ $t('common.panelIntro.purpose') }}</span>
      <p>{{ purpose }}</p>
    </div>
    <div class="intro-item">
      <span class="intro-label">{{ $t('common.panelIntro.how') }}</span>
      <p>{{ how }}</p>
    </div>
  </section>
</template>

<style scoped>
.panel-intro {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
  gap: 12px;
  padding: 14px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-secondary);
}

.intro-item {
  min-width: 0;
}

.intro-label {
  display: block;
  margin-bottom: 4px;
  color: var(--text-dim);
  font-size: 12px;
}

.intro-item p {
  margin: 0;
  color: var(--text-secondary);
  font-size: 13px;
  line-height: 1.5;
}
</style>