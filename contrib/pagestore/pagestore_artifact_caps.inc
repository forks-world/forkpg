/*
 * Leaf-locked control-era registrations. BEGIN replaces only the unfinished
 * attempt of that key; durable COMMIT caps survive until their generation's
 * physical versions are retired. Recovery reconstructs committed caps only.
 */
typedef struct ArtifactCap
{
	uint32_t	timeline;
	PsKey		key;
	uint64_t	lsn,
				token;
	uint64_t	commit_seq;
} ArtifactCap;
static ArtifactCap *artifact_caps;
static uint32_t nartifact_caps;
static int	artifact_caps_poisoned;
static pthread_mutex_t artifact_caps_lock = PTHREAD_MUTEX_INITIALIZER;

static int
artifact_cap_note(uint32_t tl, const PsKey *key, uint64_t lsn, uint64_t token,
				  uint64_t commit_seq)
{
	int			rc = -1;

	pthread_mutex_lock(&artifact_caps_lock);
	if (artifact_caps_poisoned)
		goto out;
	for (uint32_t i = 0; i < nartifact_caps;)
	{
		ArtifactCap *c = &artifact_caps[i];

		if (c->timeline == tl && key_eq(&c->key, key))
		{
			if (c->lsn == lsn && c->token == token)
			{
				if (commit_seq)
					c->commit_seq = commit_seq;
				rc = 0;
				goto out;
			}
			if (!c->commit_seq)
			{
				*c = artifact_caps[--nartifact_caps];
				continue;
			}
		}
		i++;
	}
	{
		ArtifactCap *grown = realloc(artifact_caps,
									 (size_t) (nartifact_caps + 1) * sizeof(*grown));

		if (!grown)
		{
			artifact_caps_poisoned = 1;
			goto out;
		}
		artifact_caps = grown;
		artifact_caps[nartifact_caps++] = (ArtifactCap)
		{
			tl, *key, lsn, token, commit_seq
		};
	}
	rc = 0;
out:
	pthread_mutex_unlock(&artifact_caps_lock);
	return rc;
}

static void
artifact_cap_drop_inflight(uint32_t tl, const PsKey *key)
{
	int			removed = 0;

	pthread_mutex_lock(&artifact_caps_lock);
	for (uint32_t i = 0; i < nartifact_caps;)
		if (artifact_caps[i].timeline == tl && !artifact_caps[i].commit_seq &&
			key_eq(&artifact_caps[i].key, key))
		{
			artifact_caps[i] = artifact_caps[--nartifact_caps];
			removed = 1;
		}
		else
			i++;
	pthread_mutex_unlock(&artifact_caps_lock);
	if (removed)
		page_prune_mark_all_due();
}

static void
artifact_cap_forget_commit(uint32_t tl, const PsKey *meta, uint64_t commit_seq)
{
	int			removed = 0;
	PsKey		key = *meta;

	key.klass = key.forkNum;
	key.forkNum = 0;
	pthread_mutex_lock(&artifact_caps_lock);
	for (uint32_t i = 0; i < nartifact_caps;)
		if (artifact_caps[i].timeline == tl && artifact_caps[i].commit_seq == commit_seq &&
			key_eq(&artifact_caps[i].key, &key))
		{
			artifact_caps[i] = artifact_caps[--nartifact_caps];
			removed = 1;
		}
		else
			i++;
	pthread_mutex_unlock(&artifact_caps_lock);
	if (removed)
		page_prune_mark_all_due_locked();
}

static void
artifact_cap_retire(uint32_t tl, uint64_t lsn, bool all)
{
	pthread_mutex_lock(&artifact_caps_lock);
	for (uint32_t i = 0; i < nartifact_caps;)
		if (artifact_caps[i].timeline == tl && (all || artifact_caps[i].lsn == lsn))
			artifact_caps[i] = artifact_caps[--nartifact_caps];
		else
			i++;
	pthread_mutex_unlock(&artifact_caps_lock);
}

static void
artifact_caps_clear(void)
{
	pthread_mutex_lock(&artifact_caps_lock);
	free(artifact_caps);
	artifact_caps = NULL;
	nartifact_caps = 0;
	artifact_caps_poisoned = 0;
	pthread_mutex_unlock(&artifact_caps_lock);
}

static int
artifact_cap_fences(uint32_t tl, PsHistoryFence **fences, uint32_t *n)
{
	int			rc = -1;

	pthread_mutex_lock(&artifact_caps_lock);
	if (artifact_caps_poisoned)
		goto out;
	if (nartifact_caps)
	{
		PsHistoryFence *grown = realloc(*fences,
										(size_t) (*n + nartifact_caps) * sizeof(**fences));

		if (!grown)
			goto out;
		*fences = grown;
		for (uint32_t i = 0; i < nartifact_caps; i++)
		{
			ArtifactCap *a = &artifact_caps[i];
			uint64_t	seq = cap_effective_seq(CAP_ARTIFACT, a->token);
			ViewCap		cap = {a->lsn, seq, seq, seq == PS_SEQ_UNBOUNDED};

			if (retention_project_cap(a->timeline, tl, &cap))
				(*fences)[(*n)++] = (PsHistoryFence)
			{
				cap.lsn, cap.strict_seq, cap.seq
			};
		}
	}
	rc = 0;
out:
	pthread_mutex_unlock(&artifact_caps_lock);
	return rc;
}

static int
artifact_cap_registered(uint32_t tl, const ViewCap *cap)
{
	int			found = 0;

	pthread_mutex_lock(&artifact_caps_lock);
	if (!artifact_caps_poisoned)
		for (uint32_t i = 0; i < nartifact_caps; i++)
		{
			ArtifactCap *a = &artifact_caps[i];
			uint64_t	seq = cap_effective_seq(CAP_ARTIFACT, a->token);
			ViewCap		projected = {a->lsn, seq, seq, seq == PS_SEQ_UNBOUNDED};

			if (retention_project_cap(a->timeline, tl, &projected) &&
				projected.lsn == cap->lsn && projected.seq == cap->seq &&
				projected.strict_seq == cap->strict_seq)
			{
				found = 1;
				break;
			}
		}
	pthread_mutex_unlock(&artifact_caps_lock);
	return found;
}

/*
 * An artifact token binds CONTROL to its own activation epoch. Older completed
 * artifacts keep their LSN-only era even when read by a P5 producer retry.
 */
static ViewCap
artifact_control_cap(uint32_t tl, uint64_t lsn, uint64_t token)
{
	ViewCap		cap = viewcap_from_request(lsn, token);

	/* Legacy CONTROL pins were LSN-only, including at their boundary. */
	if (cap.legacy)
		cap.strict_seq = PS_SEQ_UNBOUNDED;

	pthread_mutex_lock(&artifact_caps_lock);
	if (!artifact_caps_poisoned)
		for (uint32_t i = 0; i < nartifact_caps; i++)
			if (artifact_caps[i].timeline == tl && artifact_caps[i].lsn == lsn &&
				artifact_caps[i].token == token)
			{
				cap.seq = cap.strict_seq = cap_effective_seq(CAP_ARTIFACT, token);
				cap.legacy = cap.seq == PS_SEQ_UNBOUNDED;
				break;
			}
	pthread_mutex_unlock(&artifact_caps_lock);
	return cap;
}
