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
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import androidx.recyclerview.widget.ConcatAdapter
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.LinearLayoutManager
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
    // Kept: in the console look these grid views are not on screen, and
    // findViewById would not find them.
    private lateinit var fab: ExtendedFloatingActionButton
    private lateinit var toolbarView: MaterialToolbar

    private val io = Executors.newSingleThreadExecutor()
    /** The console-style library (carousel and list), when that look is chosen. */
    private var consoleUi: ConsoleUi? = null
    private var games: List<Game> = emptyList()
    private var filter: String = FILTER_ALL
    private var query: String = ""
    private val continueRow = ContinueRowAdapter()
    // Play time per game uri, read in the background (small files).
    private var playTimes: Map<String, Long> = emptyMap()
    private lateinit var info: GameInfo
    // Year and genre per game uri, from the packed database.
    private var facts: Map<String, GameInfo.Facts> = emptyMap()

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
        ErrorReport.install(this)
        setContentView(R.layout.activity_library)
        store = LibraryStore(this)
        covers = Covers(this)
        info = GameInfo(this)

        val toolbar = findViewById<MaterialToolbar>(R.id.toolbar)
        toolbar.inflateMenu(R.menu.library)
        toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_settings -> startActivity(Intent(this, SettingsActivity::class.java))
                R.id.action_open_file -> pickFile.launch(arrayOf("*/*"))
                R.id.action_folders -> showFolders()
                R.id.action_rescan -> rescan()
                R.id.action_console_style -> setStyle("console")
                R.id.action_report -> ErrorReport.show(this)
                R.id.action_refresh_covers -> {
                    io.execute {
                        covers.refresh()
                        runOnUiThread { adapter.notifyDataSetChanged() }
                    }
                }
            }
            true
        }
        (toolbar.menu.findItem(R.id.action_search).actionView as SearchView).apply {
            queryHint = getString(R.string.search_hint)
            setOnQueryTextListener(object : SearchView.OnQueryTextListener {
                override fun onQueryTextSubmit(q: String?) = true
                override fun onQueryTextChange(q: String?): Boolean {
                    this@LibraryActivity.query = q.orEmpty().trim()
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
        grid.adapter = ConcatAdapter(continueRow, adapter)
        grid.layoutManager = GridLayoutManager(this, spanCount()).apply {
            spanSizeLookup = object : GridLayoutManager.SpanSizeLookup() {
                // The "continue" row spans the whole width.
                override fun getSpanSize(position: Int) =
                    if (position < continueRow.itemCount) spanCount else 1
            }
        }

        fab = findViewById(R.id.fab)
        toolbarView = toolbar
        fab.setOnClickListener { addFolder() }
        findViewById<View>(R.id.empty_add).setOnClickListener { addFolder() }
        findViewById<View>(R.id.empty_open).setOnClickListener { pickFile.launch(arrayOf("*/*")) }

        games = store.loadGames()
        buildChips()
        applyFilter()
        if (store.folders.isNotEmpty() || store.looseGames().isNotEmpty()) rescan()
        // Automated screenshots (CI emulator): a game folder by path.
        intent.getStringExtra("cartucho_test_folder")?.let { path ->
            store.addFolder(Uri.fromFile(java.io.File(path)).toString())
            rescan()
        }
        intent.getStringExtra("cartucho_test_style")?.let { style ->
            androidx.preference.PreferenceManager.getDefaultSharedPreferences(this).edit().putString("ui_style", style).apply()
        }
        if (Prefs(this).uiStyle == "console") startConsoleUi()
        handlePlayIntent(intent)
        if (intent.getStringExtra("cartucho_test_folder") == null) Updater.checkOnStart(this)
    }

    private fun startConsoleUi() {
        val ui = ConsoleUi(this, object : ConsoleUi.Host {
            override val covers get() = this@LibraryActivity.covers
            override val info get() = this@LibraryActivity.info
            override val store get() = this@LibraryActivity.store
            override fun games() = games
            override fun facts(game: Game) = facts[game.uri]
            override fun playTime(game: Game) = playTimes[game.uri] ?: 0L
            override fun launch(game: Game) = this@LibraryActivity.launch(game)
            override fun gameMenu(game: Game) = showGameMenu(game)
            override fun mainMenu() = consoleMenu()
            override fun search(onQuery: (String) -> Unit) = askSearch(onQuery)
        })
        consoleUi = ui
        setContentView(ui.root)
        onBackPressedDispatcher.addCallback(this, object : androidx.activity.OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (!ui.onBack()) {
                    isEnabled = false
                    onBackPressedDispatcher.onBackPressed()
                    isEnabled = true
                }
            }
        })
        ui.refresh()
    }

    /** START / Menu in the console look. */
    private fun consoleMenu() {
        val items = listOf<Pair<String, () -> Unit>>(
            "⚙️ Configurações" to { startActivity(Intent(this, SettingsActivity::class.java)) },
            "📁 Adicionar pasta de jogos" to { addFolder() },
            "🗂 Pastas" to { showFolders() },
            "📄 Abrir um arquivo" to { pickFile.launch(arrayOf("*/*")) },
            "🔄 Procurar jogos de novo" to { rescan() },
            "🖼 Atualizar capas" to { io.execute { covers.refresh(); runOnUiThread { applyFilter() } } },
            "▦ Biblioteca em grade de capas" to { setStyle("grid") },
            "🩺 Relatório de erro" to { ErrorReport.show(this) },
        )
        MaterialAlertDialogBuilder(this)
            .setTitle("Cartucho")
            .setItems(items.map { it.first }.toTypedArray()) { _, which -> items[which].second() }
            .show()
    }

    private fun setStyle(style: String) {
        androidx.preference.PreferenceManager.getDefaultSharedPreferences(this).edit().putString("ui_style", style).apply()
        recreate()
    }

    private fun askSearch(onQuery: (String) -> Unit) {
        val input = android.widget.EditText(this).apply {
            hint = getString(R.string.search_hint)
            isSingleLine = true
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("Buscar jogo")
            .setView(android.widget.FrameLayout(this).apply { setPadding(48, 16, 48, 0); addView(input) })
            .setPositiveButton("Buscar") { _, _ ->
                input.text.toString().trim().takeIf { it.isNotEmpty() }?.let(onQuery)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        input.requestFocus()
    }

    /** A short message: a snackbar in the grid, a toast in the console look. */
    private fun notice(text: String) {
        if (consoleUi == null) Snackbar.make(grid, text, Snackbar.LENGTH_LONG).show()
        else android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_LONG).show()
    }

    override fun onPause() {
        super.onPause()
        consoleUi?.pause()
    }

    override fun onDestroy() {
        super.onDestroy()
        consoleUi?.release()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handlePlayIntent(intent)
    }

    /** A home-screen shortcut to one game. */
    private fun handlePlayIntent(intent: Intent?) {
        if (intent?.action != ACTION_PLAY) return
        val uri = intent.getStringExtra(EXTRA_GAME_URI) ?: return
        intent.action = null  // not again on rotation
        val game = (games + store.looseGames()).firstOrNull { it.uri == uri }
        if (game == null) {
            notice("Esse jogo não está mais na biblioteca.")
            return
        }
        launch(game)
    }

    override fun onResume() {
        super.onResume()
        // Coming back from a game that closed by itself: offer the report.
        if (ErrorReport.hasNew(this)) {
            if (consoleUi == null) Snackbar.make(grid, "O último jogo teve um problema.", Snackbar.LENGTH_LONG)
                .setAction("Ver relatório") { ErrorReport.show(this) }
                .show()
            else MaterialAlertDialogBuilder(this)
                .setMessage("O último jogo teve um problema.")
                .setPositiveButton("Ver relatório") { _, _ -> ErrorReport.show(this) }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
        consoleUi?.resume()
        // "Recentes" order, play times and the continue row change after playing.
        refreshPlayInfo()
        // Saves in the cloud: a fresh copy after playing.
        io.execute { CloudSync.sendIfChanged(this) }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        (grid.layoutManager as GridLayoutManager).spanCount = spanCount()
    }

    private fun spanCount(): Int {
        val widthDp = resources.configuration.screenWidthDp
        return (widthDp / 170).coerceIn(2, 8)
    }

    private var chipSystems: Set<GameSystem>? = null

    /** Filters: all / recent / favourites, then one per console that has games. */
    private fun buildChips() {
        val present = games.map { it.system }.toSet()
        if (present == chipSystems) return
        chipSystems = present
        chips.removeAllViews()
        if (filter != FILTER_ALL && filter != FILTER_RECENT && filter != FILTER_FAV &&
            present.none { it.name == filter }) filter = FILTER_ALL
        val options = listOf(
            FILTER_ALL to getString(R.string.filter_all),
            FILTER_RECENT to getString(R.string.filter_recent),
            FILTER_FAV to getString(R.string.filter_favorites),
        ) + GameSystem.entries.filter { it in present }.map { it.name to it.shortName }
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
        updateContinueRow()
        consoleUi?.refresh()
        val nothingAtAll = games.isEmpty() && progress.visibility != View.VISIBLE
        empty.visibility = if (nothingAtAll) View.VISIBLE else View.GONE
        fab.visibility = if (nothingAtAll) View.GONE else View.VISIBLE
    }

    private fun refreshPlayInfo() {
        val all = games
        io.execute {
            val times = all.associate { it.uri to PlayStats.playTime(this, it) }.filterValues { it > 0 }
            val known = all.mapNotNull { g -> info.quickFacts(g)?.let { g.uri to it } }.toMap()
            runOnUiThread {
                playTimes = times
                facts = known
                applyFilter()
                adapter.notifyDataSetChanged()
            }
        }
    }

    /** Shown on the full list only, without a search: up to 10 recent games. */
    private fun updateContinueRow() {
        val show = filter == FILTER_ALL && query.isEmpty()
        val recent = if (!show) emptyList() else games
            .filter { store.lastPlayed(it) > 0 }
            .sortedByDescending { store.lastPlayed(it) }
            .take(10)
        continueRow.setGames(recent)
    }

    private fun rescan() {
        progress.visibility = View.VISIBLE
        io.execute {
            val found = store.scan()
            runOnUiThread {
                games = found
                progress.visibility = View.GONE
                buildChips()
                applyFilter()
                refreshPlayInfo()
                if (found.isNotEmpty())
                    supportActionBarTitle(getString(R.string.games_found, found.size))
            }
        }
    }

    private fun supportActionBarTitle(subtitle: String) {
        toolbarView.subtitle = subtitle
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
        if (Archives.isArchive(name)) {
            val archiveName = name
            io.execute {
                val inner = Archives.peek(this, uri, archiveName)
                runOnUiThread {
                    val innerName = inner?.name?.substringAfterLast('/')
                    val sys = innerName?.let { GameSystem.forFileName(it) }
                    if (inner == null || sys == null) {
                        notice(getString(R.string.unsupported_file))
                        return@runOnUiThread
                    }
                    runCatching {
                        contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    val game = Game(uri.toString(), innerName, sys, size, inner.name, archiveName)
                    store.addLoose(game)
                    if (games.none { it.uri == game.uri }) {
                        games = (games + game).sortedBy { it.title.lowercase() }
                        applyFilter()
                    }
                    launch(game)
                }
            }
            return
        }
        if (name.endsWith(".cia", ignoreCase = true)) {
            notice(getString(R.string.cia_not_supported))
            return
        }
        val system = GameSystem.forFileName(name, size)
        if (system == null) {
            notice(getString(R.string.unsupported_file))
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
        if (game.system.needsPath && game.entry == null && Storage.directPath(this, game) == null) {
            ask3dsFileAccess(game)
            return
        }
        store.markPlayed(game)
        startActivity(GameActivity.intent(this, game))
        // Long-pressing the app icon lists the latest games.
        io.execute { runCatching { ShortcutManagerCompat.pushDynamicShortcut(this, shortcut(game)) } }
    }

    /** Shortcut opening this game directly (built off the main thread: it may fetch the cover). */
    private fun shortcut(game: Game): ShortcutInfoCompat {
        val icon = covers.bitmapBlocking(game)?.let { bmp ->
            val side = minOf(bmp.width, bmp.height)
            val square = android.graphics.Bitmap.createBitmap(bmp, (bmp.width - side) / 2, (bmp.height - side) / 2, side, side)
            IconCompat.createWithBitmap(android.graphics.Bitmap.createScaledBitmap(square, 192, 192, true))
        } ?: IconCompat.createWithResource(this, R.mipmap.ic_launcher)
        val intent = Intent(this, LibraryActivity::class.java)
            .setAction(ACTION_PLAY)
            .putExtra(EXTRA_GAME_URI, game.uri)
        return ShortcutInfoCompat.Builder(this, "game:" + game.uri.hashCode())
            .setShortLabel(game.title.take(24))
            .setLongLabel(game.title)
            .setIcon(icon)
            .setIntent(intent)
            .build()
    }

    private fun pinShortcut(game: Game) {
        if (!ShortcutManagerCompat.isRequestPinShortcutSupported(this)) {
            notice("A tela inicial deste celular não aceita atalhos.")
            return
        }
        io.execute {
            val info = shortcut(game)
            runOnUiThread { ShortcutManagerCompat.requestPinShortcut(this, info, null) }
        }
    }

    private val legacyStoragePermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    /** 3DS games are opened in place; that needs broad file access once. */
    private fun ask3dsFileAccess(game: Game) {
        if (Storage.realPath(game.uri) == null) {
            MaterialAlertDialogBuilder(this)
                .setTitle(game.title)
                .setMessage("Jogos de 3DS e PlayStation precisam estar na memória interna ou no cartão SD " +
                    "(não em Google Drive ou outros apps). Mova o jogo e adicione a pasta de novo.")
                .setPositiveButton(R.string.ok, null)
                .show()
            return
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("Permitir acesso aos jogos de ${game.system.fullName}")
            .setMessage("Jogos de ${game.system.fullName} são grandes, então o Cartucho abre direto da pasta em " +
                "vez de copiar. Para isso o Android pede a permissão \"Acesso a todos os " +
                "arquivos\". Ative na próxima tela e volte para o app.")
            .setPositiveButton("Abrir configuração") { _, _ ->
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                    try {
                        startActivity(Storage.allFilesAccessIntent(this))
                    } catch (e: Exception) {
                        startActivity(Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
                    }
                } else {
                    legacyStoragePermission.launch(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /** The system folder picker; some TVs do not have one. */
    private fun addFolder() {
        try {
            pickFolder.launch(null)
        } catch (e: android.content.ActivityNotFoundException) {
            // TVs without the picker: a fixed folder, read with all-files access.
            val dir = java.io.File(android.os.Environment.getExternalStorageDirectory(), "Cartucho")
            MaterialAlertDialogBuilder(this)
                .setTitle("Sem seletor de pastas")
                .setMessage("Este aparelho não tem o seletor de pastas do Android (comum em TVs). " +
                    "Coloque os jogos na pasta \"Cartucho\" da memória (${dir.path}) — por pendrive, " +
                    "app de arquivos ou pelo computador — e toque em Usar essa pasta.")
                .setPositiveButton("Usar essa pasta") { _, _ ->
                    if (!Storage.hasFileAccess(this)) {
                        runCatching { startActivity(Storage.allFilesAccessIntent(this)) }
                        notice("Permita o acesso a todos os arquivos e toque em Adicionar pasta de novo.")
                    } else {
                        dir.mkdirs()
                        store.addFolder(Uri.fromFile(dir).toString())
                        rescan()
                    }
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
    }

    private fun showFolders() {
        val folders = store.folders
        if (folders.isEmpty()) {
            addFolder()
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
            .setPositiveButton(R.string.add_folder) { _, _ -> addFolder() }
            .show()
    }

    private var coverFor: Game? = null
    private val pickCover = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val game = coverFor ?: return@registerForActivityResult
        uri ?: return@registerForActivityResult
        io.execute {
            val ok = covers.setCustom(game, uri)
            runOnUiThread {
                if (!ok) notice("Não foi possível usar essa imagem.")
                adapter.notifyDataSetChanged()
            }
        }
    }

    private fun showGameMenu(game: Game) {
        val fav = store.isFavorite(game)
        val actions = mutableListOf<Pair<String, () -> Unit>>(
            "ℹ️ Detalhes do jogo" to { showDetails(game) },
            getString(if (fav) R.string.unfavorite else R.string.favorite) to {
                store.setFavorite(game, !fav)
                applyFilter()
                adapter.notifyDataSetChanged()
            },
            "Escolher capa da galeria" to {
                coverFor = game
                pickCover.launch(arrayOf("image/*"))
            },
        )
        actions += "Criar atalho na tela inicial" to { pinShortcut(game) }
        if (PlayStats.hasResume(this, game)) actions += "Começar do zero na próxima vez" to {
            PlayStats.clearResume(this, game)
            updateContinueRow()
        }
        if (covers.hasCustom(game)) actions += "Remover capa escolhida" to {
            covers.removeCustom(game)
            adapter.notifyDataSetChanged()
        }
        val time = playTimes[game.uri] ?: 0L
        MaterialAlertDialogBuilder(this)
            .setTitle(game.title + if (time > 0) "  ·  ${PlayStats.formatTime(time)}" else "")
            .setItems(actions.map { it.first }.toTypedArray()) { _, which -> actions[which].second() }
            .show()
    }

    /** Cover, facts and description of one game. */
    private fun showDetails(game: Game) {
        val pad = (20 * resources.displayMetrics.density).toInt()
        val cover = ImageView(this).apply {
            scaleType = ImageView.ScaleType.FIT_CENTER
            adjustViewBounds = true
        }
        val factsView = TextView(this).apply {
            setTextColor(ContextCompat.getColor(this@LibraryActivity, R.color.on_surface))
            textSize = 15f
            setPadding(0, pad / 2, 0, 0)
            text = game.system.fullName
        }
        val description = TextView(this).apply {
            setTextColor(ContextCompat.getColor(this@LibraryActivity, R.color.on_surface_dim))
            textSize = 14f
            setPadding(0, pad / 2, 0, 0)
        }
        val box = android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            setPadding(pad, pad / 2, pad, 0)
            addView(cover, android.widget.LinearLayout.LayoutParams(
                android.widget.LinearLayout.LayoutParams.MATCH_PARENT, (180 * resources.displayMetrics.density).toInt()))
            addView(factsView)
            addView(description)
        }
        covers.load(game, cover) { found -> cover.visibility = if (found) View.VISIBLE else View.GONE }
        MaterialAlertDialogBuilder(this)
            .setTitle(game.title)
            .setView(android.widget.ScrollView(this).apply { addView(box) })
            .setPositiveButton("Jogar") { _, _ -> launch(game) }
            .setNegativeButton("Fechar", null)
            .show()
        io.execute {
            val f = info.facts(game)
            val time = PlayStats.playTime(this, game)
            val lines = mutableListOf(game.system.fullName + (game.tags.takeIf { it.isNotEmpty() }?.let { " · $it" } ?: ""))
            if (f != null) {
                if (f.year.isNotEmpty()) lines += "Ano: ${f.year}"
                if (f.genre.isNotEmpty()) lines += "Gênero: ${f.genre}"
                if (f.developer.isNotEmpty()) lines += "Desenvolvedora: ${f.developer}"
                if (f.publisher.isNotEmpty() && f.publisher != f.developer) lines += "Distribuidora: ${f.publisher}"
                if (f.players.isNotEmpty()) lines += if (f.players == "1") "1 jogador" else "Até ${f.players} jogadores"
            }
            if (time > 0) lines += "Tempo jogado: ${PlayStats.formatTime(time)}"
            val kept = info.keptDescription(game)
            runOnUiThread {
                factsView.text = lines.joinToString("\n")
                description.text = kept ?: "Buscando descrição…"
            }
            if (kept == null) {
                val text = info.fetchDescription(game, covers)
                runOnUiThread {
                    description.text = text ?: "Sem descrição. Configure o ScreenScraper (descrições em português) " +
                        "ou o TheGamesDB nas Configurações para ver o resumo dos jogos."
                }
            }
        }
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
            val time = playTimes[game.uri]
            val f = facts[game.uri]
            subtitle.text = when {
                time != null -> listOfNotNull(game.system.shortName, f?.year?.ifEmpty { null },
                    "${PlayStats.formatTime(time)} jogados").joinToString(" · ")
                f != null -> listOfNotNull(game.system.shortName, f.year.ifEmpty { null },
                    f.genre.substringBefore(" (").ifEmpty { null }).joinToString(" · ")
                else -> listOf(game.system.fullName, game.tags).filter { it.isNotEmpty() }.joinToString(" · ")
            }
            covers.load(game, cover) { found -> initials.visibility = if (found) View.INVISIBLE else View.VISIBLE }
            itemView.setOnClickListener { launch(game) }
            itemView.setOnLongClickListener { showGameMenu(game); true }
            focusGrow(itemView)
        }
    }

    /** The "Continuar jogando" header: one full-width item holding a horizontal list. */
    private inner class ContinueRowAdapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
        private var list: List<Game> = emptyList()
        private val cards = ContinueCardAdapter()

        fun setGames(games: List<Game>) {
            val had = list.isNotEmpty()
            list = games
            cards.submitList(games)
            when {
                had && games.isEmpty() -> notifyItemRemoved(0)
                !had && games.isNotEmpty() -> notifyItemInserted(0)
                games.isNotEmpty() -> notifyItemChanged(0)
            }
        }

        override fun getItemCount() = if (list.isEmpty()) 0 else 1
        override fun getItemViewType(position: Int) = 1

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
            val view = LayoutInflater.from(parent.context).inflate(R.layout.item_continue_row, parent, false)
            view.findViewById<RecyclerView>(R.id.continue_list).apply {
                layoutManager = LinearLayoutManager(context, LinearLayoutManager.HORIZONTAL, false)
                adapter = cards
            }
            return object : RecyclerView.ViewHolder(view) {}
        }

        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            cards.notifyDataSetChanged()
        }
    }

    private inner class ContinueCardAdapter : ListAdapter<Game, ContinueHolder>(DIFF) {
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
            ContinueHolder(LayoutInflater.from(parent.context).inflate(R.layout.item_continue, parent, false))

        override fun onBindViewHolder(holder: ContinueHolder, position: Int) = holder.bind(getItem(position))
    }

    private inner class ContinueHolder(view: View) : RecyclerView.ViewHolder(view) {
        private val shot: ImageView = view.findViewById(R.id.shot)
        private val badge: TextView = view.findViewById(R.id.badge)
        private val play: TextView = view.findViewById(R.id.play)
        private val title: TextView = view.findViewById(R.id.title)
        private val info: TextView = view.findViewById(R.id.info)

        fun bind(game: Game) {
            val color = ContextCompat.getColor(itemView.context, game.system.color)
            badge.text = game.system.shortName
            play.background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(color)
            }
            title.text = game.title
            val parts = mutableListOf(PlayStats.formatAgo(store.lastPlayed(game)))
            playTimes[game.uri]?.let { parts += PlayStats.formatTime(it) }
            info.text = parts.joinToString(" · ")
            shot.setImageDrawable(null)
            shot.tag = game.uri
            io.execute {
                // Where the player stopped; the cover when there is no picture yet.
                val thumb = PlayStats.thumb(this@LibraryActivity, game)
                runOnUiThread {
                    if (shot.tag != game.uri) return@runOnUiThread
                    if (thumb != null) {
                        // Pixel art stays sharp when enlarged.
                        shot.scaleType = ImageView.ScaleType.FIT_CENTER
                        shot.setImageDrawable(android.graphics.drawable.BitmapDrawable(resources, thumb).apply {
                            isFilterBitmap = thumb.width > 300
                        })
                    } else {
                        shot.scaleType = ImageView.ScaleType.CENTER_CROP
                        covers.load(game, shot) { }
                    }
                }
            }
            itemView.setOnClickListener { launch(game) }
            itemView.setOnLongClickListener { showGameMenu(game); true }
            focusGrow(itemView)
        }
    }

    /** TV remote / controller: the focused card grows a little (and gets its outline). */
    private fun focusGrow(v: View) {
        v.setOnFocusChangeListener { view, focused ->
            view.animate().scaleX(if (focused) 1.06f else 1f).scaleY(if (focused) 1.06f else 1f)
                .translationZ(if (focused) 8f else 0f).setDuration(120).start()
        }
    }

    /**
     * Controller in the library: Y or X opens the game's menu (like a long
     * press), L1 / R1 move between the console filters. A and B already act
     * as select / back.
     */
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        consoleUi?.let { if (it.onKey(event)) return true else return super.dispatchKeyEvent(event) }
        if (event.action == android.view.KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
            when (event.keyCode) {
                android.view.KeyEvent.KEYCODE_BUTTON_Y, android.view.KeyEvent.KEYCODE_BUTTON_X,
                android.view.KeyEvent.KEYCODE_MENU -> {
                    currentFocus?.performLongClick()?.let { if (it) return true }
                }
                android.view.KeyEvent.KEYCODE_BUTTON_L1, android.view.KeyEvent.KEYCODE_BUTTON_R1 -> {
                    val n = chips.childCount
                    if (n > 0) {
                        val at = (0 until n).indexOfFirst { (chips.getChildAt(it) as Chip).isChecked }.coerceAtLeast(0)
                        val next = (at + if (event.keyCode == android.view.KeyEvent.KEYCODE_BUTTON_R1) 1 else n - 1) % n
                        (chips.getChildAt(next) as Chip).performClick()
                        return true
                    }
                }
            }
        }
        return super.dispatchKeyEvent(event)
    }

    companion object {
        const val ACTION_PLAY = "com.cartucho.emu.PLAY"
        const val EXTRA_GAME_URI = "game_uri"
        private const val FILTER_ALL = "all"
        private const val FILTER_RECENT = "recent"
        private const val FILTER_FAV = "fav"

        private val DIFF = object : DiffUtil.ItemCallback<Game>() {
            override fun areItemsTheSame(a: Game, b: Game) = a.uri == b.uri
            override fun areContentsTheSame(a: Game, b: Game) = a == b
        }
    }
}
