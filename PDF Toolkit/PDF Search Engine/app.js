const express = require('express');
const path = require('path');
const { exec } = require('child_process');
const glob = require('glob');
const app = express();
const port = 3000;

// Set the view engine for HTML rendering
app.set('view engine', 'html');
app.use(express.static(path.join(__dirname, 'views')));
app.use(express.urlencoded({ extended: true }));

// Route for rendering the HTML page
app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, '/views/index.html'));
});

// Route for handling the PDF search
app.post('/search', (req, res) => {
    const directory = req.body.directory;
    const keyword = req.body.keyword;

    // Use glob to find all PDF files in the given directory
    glob(`${directory}/**/*.pdf`, (err, files) => {
        if (err) {
            console.error(`Glob error: ${err}`);
            return res.status(500).send(`Error: ${err.message}`);
        }

        if (files.length === 0) {
            return res.status(404).send('No PDF files found in the specified directory.');
        }

        // Create a space-separated string of all matched PDF files
        const fileList = files.join(' ');

        // Run pdfgrep on the list of matched files
        exec(`pdfgrep "${keyword}" ${fileList}`, (error, stdout, stderr) => {
            if (error) {
                console.error(`Error executing command: ${error.message}`);
                return res.status(500).send(`Error: ${error.message}`);
            }
            if (stderr) {
                console.error(`Error output: ${stderr}`);
                return res.status(500).send(`Error: ${stderr}`);
            }

            // Return search results
            res.send(`
                <h3>Search Results for "${keyword}":</h3>
                <pre>${stdout || 'No matches found'}</pre>
                <br><a href="/">Go back</a>
            `);
        });
    });
});

// Start the server
app.listen(port, () => {
    console.log(`Server running on http://localhost:${port}`);
});
