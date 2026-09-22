import { computed, ref, watch, type Ref } from 'vue'

/**
 * UX-PIPE-9 (docs/PHASE_TRACKER.md, 18/09/2026) : affichage progressif +
 * recherche pour une liste conservée en entier en mémoire/stockage mais
 * dont seule une tranche fixe (12, 20...) était rendue sans aucun moyen
 * d'atteindre le reste (pas de bouton suivant, pas de recherche). Chaque
 * appel crée un état INDÉPENDANT (filtre + nombre visible propres à cette
 * liste) -- ne pas réutiliser la même instance pour deux listes différentes.
 *
 * `visibleItems` est un simple `slice(0, visibleCount)` : contrairement à
 * une pagination par numéro de page, une suppression dans la collection ne
 * peut jamais produire une page "hors bornes" à corriger explicitement --
 * le slice retourne naturellement moins d'éléments, jamais une erreur.
 */
export function usePaginatedFilter<T>(
  items: Ref<T[]>,
  pageSize: number,
  matchText: (item: T) => string,
) {
  const filterText = ref('')
  const visibleCount = ref(pageSize)

  const filteredItems = computed(() => {
    const query = filterText.value.trim().toLowerCase()
    if (!query) return items.value
    return items.value.filter((item) => matchText(item).toLowerCase().includes(query))
  })

  const visibleItems = computed(() => filteredItems.value.slice(0, visibleCount.value))

  // Une nouvelle recherche repart de la première page de résultats -- sinon
  // un nombre visible élevé hérité d'une recherche précédente pourrait
  // afficher d'un coup une collection filtrée bien plus petite sans qu'on
  // l'ait demandé, ou masquer qu'un filtre très large a été appliqué.
  watch(filterText, () => {
    visibleCount.value = pageSize
  })

  function showMore() {
    visibleCount.value += pageSize
  }

  return {
    filterText,
    visibleItems,
    filteredCount: computed(() => filteredItems.value.length),
    totalCount: computed(() => items.value.length),
    hasMore: computed(() => visibleItems.value.length < filteredItems.value.length),
    showMore,
  }
}
