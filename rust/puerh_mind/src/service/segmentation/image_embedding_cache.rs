//! Image embedding cache of the segmentation engine (SM3.B plan, decisions B1, B2, and B6).
//!
//! The cache maps the opaque image key of the host to the shared image embedding of that image.
//! It keeps at most `capacity` entries and removes the least recently used entry first. A hit and
//! an insert make the entry the most recent. The engine clears the cache on an unload and on a
//! model change.

/// Least-recently-used map from an image key to a shared value. The capacity is small (4), so the
/// entries are a list in recency order: the most recent entry is last.
pub struct ImageEmbeddingCache<V> {
    capacity: usize,
    entries: Vec<(String, V)>,
}

impl<V: Clone> ImageEmbeddingCache<V> {
    /// An empty cache with room for @p capacity entries (at least 1).
    pub fn new(capacity: usize) -> Self {
        let capacity = capacity.max(1);
        Self {
            capacity,
            entries: Vec::with_capacity(capacity),
        }
    }

    /// The value of @p key, or `None`. A hit makes the entry the most recent.
    pub fn get(&mut self, key: &str) -> Option<V> {
        let index = self.entries.iter().position(|(entry, _)| entry == key)?;
        let entry = self.entries.remove(index);
        let value = entry.1.clone();
        self.entries.push(entry);
        Some(value)
    }

    /// Stores @p value under @p key as the most recent entry. An entry with the same key is
    /// replaced. When the cache is full, the least recent entry leaves it.
    pub fn insert(&mut self, key: String, value: V) {
        if let Some(index) = self.entries.iter().position(|(entry, _)| *entry == key) {
            self.entries.remove(index);
        } else if self.entries.len() == self.capacity {
            self.entries.remove(0);
        }
        self.entries.push((key, value));
    }

    /// Removes every entry.
    pub fn clear(&mut self) {
        self.entries.clear();
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cache_evicts_least_recently_used_entry() {
        let mut cache = ImageEmbeddingCache::new(2);
        cache.insert("a".to_string(), 1);
        cache.insert("b".to_string(), 2);
        assert_eq!(cache.get("a"), Some(1));
        cache.insert("c".to_string(), 3);
        assert_eq!(cache.len(), 2);
        assert_eq!(cache.get("b"), None);
        assert_eq!(cache.get("a"), Some(1));
        assert_eq!(cache.get("c"), Some(3));
        cache.clear();
        assert!(cache.is_empty());
        assert_eq!(cache.get("a"), None);
    }

    #[test]
    fn cache_insert_of_existing_key_replaces_entry() {
        let mut cache = ImageEmbeddingCache::new(2);
        cache.insert("a".to_string(), 1);
        cache.insert("b".to_string(), 2);
        cache.insert("a".to_string(), 10);
        assert_eq!(cache.len(), 2);
        // "a" is now the most recent entry, so "b" leaves first.
        cache.insert("c".to_string(), 3);
        assert_eq!(cache.get("b"), None);
        assert_eq!(cache.get("a"), Some(10));
    }
}
