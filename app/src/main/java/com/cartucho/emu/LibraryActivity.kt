package com.cartucho.emu

import android.content.Intent
import android.content.res.Configuration
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SearchView
import androidx.core.content.ContextCompat
import androidx.core.graphics.ColorUtils
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.chip.Chip
import com.google.android.material.chip.ChipGroup
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.floatingactionbutton.ExtendedFloatingActionButton
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.snackbar.Snackbar
import java.util.concurrent.Executors

/** The start screen: every game in the user's folders, as a grid of covers. */
class LibraryActivity : AppCompatActivity() {

    private lateinit var store: LibraryStore
    private lateinit var covers: Covers
    private lateinit var adapter: GameAdapter
    private lateinit var grid: RecyclerView
    private lateinit var empty: View
    private lateinit var progress: LinearProgressIndicator
    private lateinit var chips: ChipGroup

    private val io = Executors.newSingleThreadExecutor()
    private var games: List<Game> = emptyList()
    private var filter: String = FILTER_ALL
    private var query: String = ""

    private val pickFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        uri ?: return@registerForActivityResult
        contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        store.addFolder(uri.toString())
        rescan()
    }

    private val pickFile = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        openSingleFile(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_library)
        store = LibraryStore(this)
        covers = Covers(this)

        val toolbar = findViewById<MaterialToolbar>(R.id.toolbar)
        toolbar.inflateMenu(R.menu.library)
        toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_settings -> startActivity(Intent(this, SettingsActivity::class.java))
                R.id.action_open_file -> pickFile.launch(arrayOf("*/*"))
                R.id.action_folders -> showFolders()
                R.id.action_rescan -> rescan()
            }
            true
        }
        (toolbar.menu.findItem(R.id.action_search).actionView as SearchView).apply {
            queryHint = getString(R.string.search_hint)
            setOnQueryTextListener(object : SearchView.OnQueryTextListener {
                override fun onQueryTextSubmit(q: String?) = true
                override fun onQueryTextChange(q: String?): Boolean {
                    query = q.orEmpty().trim()
                    applyFilter()
                    return true
                }
            })
        }

        grid = findViewById(R.id.grid)
        empty = findViewById(R.id.empty)
        progress = findViewById(R.id.progress)
        chips = findViewById(R.id.chips)
        adapter = GameAdapter()
        grid.adapter = adapter
        grid.layoutManager = GridLayoutManager(this, spanCount())

        findViewById<ExtendedFloatingActionButton>(R.id.fab).setOnClickListener { pickFolder.launch(null) }
        findViewById<View>(R.id.empty_add).setOnClickListener { pickFolder.launch(null) }
        findViewById<View>(R.id.empty_open).setOnClickListener { pickFile.launch(arrayOf("*/*")) }

        buildChips()
        games = store.loadGames()
        applyFilter()
        if (store.folders.isNotEmpty() || store.looseGames().isNotEmpty()) rescan()
    }

    override fun onResume() {
        super.onResume()
        // "Recentes" order changes after playing.
        if (filter == FILTER_RECENT) applyFilter()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        (grid.layoutManager as GridLayoutManager).spanCount = spanCount()
    }

    private fun spanCount(): Int {
        val widthDp = resources.configuration.screenWidthDp
        return (widthDp / 170).coerceIn(2, 8)
    }

    private fun buildChips() {
        val options = listOf(
            FILTER_ALL to getString(R.string.filter_all),
            FILTER_RECENT to getString(R.string.filter_recent),
            FILTER_FAV to getString(R.string.filter_favorites),
        ) + GameSystem.entries.map { it.name to it.shortName }
        for ((key, label) in options) {
            val chip = Chip(this).apply {
                text = label
                isCheckable = true
                isChecked = key == filter
                setOnClickListener {
                    filter = key
                    applyFilter()
                }
            }
            chips.addView(chip)
        }
    }

    private fun applyFilter() {
        var list = games
        list = when (filter) {
            FILTER_RECENT -> list.filter { store.lastPlayed(it) > 0 }.sortedByDescending { store.lastPlayed(it) }
            FILTER_FAV -> list.filter { store.isFavorite(it) }
            FILTER_ALL -> list
            else -> list.filter { it.system.name == filter }
        }
        if (query.isNotEmpty()) list = list.filter { it.title.contains(query, ignoreCase = true) }
        adapter.submitList(list)
        val nothingAtAll = games.isEmpty() && progress.visibility != View.VISIBLE
        empty.visibility = if (nothingAtAll) View.VISIBLE else View.GONE
        findViewById<View>(R.id.fab).visibility = if (nothingAtAll) View.GONE else View.VISIBLE
    }

    private fun rescan() {
        progress.visibility = View.VISIBLE
        io.execute {
            val found = store.scan()
            runOnUiThread {
                games = found
                progress.visibility = View.GONE
                applyFilter()
                if (found.isNotEmpty())
                    supportActionBarTitle(getString(R.string.games_found, found.size))
            }
        }
    }

    private fun supportActionBarTitle(subtitle: String) {
        findViewById<MaterialToolbar>(R.id.toolbar).subtitle = subtitle
    }

    private fun openSingleFile(uri: Uri) {
        var name = uri.lastPathSegment ?: "game"
        var size = 0L
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)
            ?.use { c ->
                if (c.moveToFirst()) {
                    name = c.getString(0) ?: name
                    size = c.getLong(1)
                }
            }
        if (name.endsWith(".cia", ignoreCase = true)) {
            Snackbar.make(grid, R.string.cia_not_supported, Snackbar.LENGTH_LONG).show()
            return
        }
        val system = GameSystem.forFileName(name)
        if (system == null) {
            Snackbar.make(grid, R.string.unsupported_file, Snackbar.LENGTH_LONG).show()
            return
        }
        runCatching {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        val game = Game(uri.toString(), name, system, size)
        store.addLoose(game)
        if (games.none { it.uri == game.uri }) {
            games = (games + game).sortedBy { it.title.lowercase() }
            applyFilter()
        }
        launch(game)
    }

    private fun launch(game: Game) {
        store.markPlayed(game)
        startActivity(GameActivity.intent(this, game))
    }

    private fun showFolders() {
        val folders = store.folders
        if (folders.isEmpty()) {
            pickFolder.launch(null)
            return
        }
        val labels = folders.map { Uri.decode(it).substringAfterLast(':').ifEmpty { it } }.toTypedArray()
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.folders)
            .setItems(labels) { _, which ->
                MaterialAlertDialogBuilder(this)
                    .setTitle(labels[which])
                    .setPositiveButton(R.string.remove_folder) { _, _ ->
                        runCatching {
                            contentResolver.releasePersistableUriPermission(
                                Uri.parse(folders[which]), Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }
                        store.removeFolder(folders[which])
                        rescan()
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
            }
            .setPositiveButton(R.string.add_folder) { _, _ -> pickFolder.launch(null) }
            .show()
    }

    private fun showGameMenu(game: Game) {
        val fav = store.isFavorite(game)
        MaterialAlertDialogBuilder(this)
            .setTitle(game.title)
            .setItems(arrayOf(getString(if (fav) R.string.unfavorite else R.string.favorite))) { _, _ ->
                store.setFavorite(game, !fav)
                applyFilter()
                adapter.notifyDataSetChanged()
            }
            .show()
    }

    private inner class GameAdapter : ListAdapter<Game, GameHolder>(DIFF) {
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): GameHolder =
            GameHolder(LayoutInflater.from(parent.context).inflate(R.layout.item_game, parent, false))

        override fun onBindViewHolder(holder: GameHolder, position: Int) = holder.bind(getItem(position))
    }

    private inner class GameHolder(view: View) : RecyclerView.ViewHolder(view) {
        private val box: ViewGroup = view.findViewById(R.id.cover_box)
        private val cover: ImageView = view.findViewById(R.id.cover)
        private val initials: TextView = view.findViewById(R.id.initials)
        private val badge: TextView = view.findViewById(R.id.badge)
        private val fav: ImageView = view.findViewById(R.id.fav)
        private val title: TextView = view.findViewById(R.id.title)
        private val subtitle: TextView = view.findViewById(R.id.subtitle)

        fun bind(game: Game) {
            val color = ContextCompat.getColor(itemView.context, game.system.color)
            box.background = GradientDrawable(
                GradientDrawable.Orientation.TL_BR,
                intArrayOf(color, ColorUtils.blendARGB(color, 0xFF0F0E17.toInt(), 0.65f)),
            )
            // Covers are roughly square for 3DS/GB and a bit wider for GBA;
            // a fixed 4:5 box keeps the grid tidy with centerCrop.
            val span = (grid.layoutManager as GridLayoutManager).spanCount
            val width = (grid.width - grid.paddingStart - grid.paddingEnd) / span
            if (width > 0) box.layoutParams.height = (width * 1.0f).toInt()
            initials.text = game.title.split(' ', '-', ':')
                .filter { it.isNotBlank() && it[0].isLetterOrDigit() }
                .take(2).joinToString("") { it.take(1).uppercase() }
            badge.text = game.system.shortName
            fav.visibility = if (store.isFavorite(game)) View.VISIBLE else View.GONE
            title.text = game.title
            subtitle.text = listOf(game.system.fullName, game.tags).filter { it.isNotEmpty() }.joinToString(" · ")
            covers.load(game, cover) { found -> initials.visibility = if (found) View.INVISIBLE else View.VISIBLE }
            itemView.setOnClickListener { launch(game) }
            itemView.setOnLongClickListener { showGameMenu(game); true }
        }
    }

    companion object {
        private const val FILTER_ALL = "all"
        private const val FILTER_RECENT = "recent"
        private const val FILTER_FAV = "fav"

        private val DIFF = object : DiffUtil.ItemCallback<Game>() {
            override fun areItemsTheSame(a: Game, b: Game) = a.uri == b.uri
            override fun areContentsTheSame(a: Game, b: Game) = a == b
        }
    }
}
